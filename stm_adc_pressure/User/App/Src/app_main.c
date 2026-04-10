#include "app_main.h"

#include "adc.h"
#include "cmsis_os2.h"
#include "comp_oled.h"
#include "drv_esp_link.h"
#include "drv_keys.h"
#include "drv_uart_log.h"
#include "spi.h"
#include "tim.h"

#include "FreeRTOS.h"
#include "task.h"

#include <stdio.h>
#include <string.h>

#define APP_ADC_CHANNEL_COUNT  6U
#define APP_ADC_DMA_FRAME_COUNT 512U
#define APP_ADC_DMA_BUFFER_SIZE (APP_ADC_CHANNEL_COUNT * APP_ADC_DMA_FRAME_COUNT)
#define APP_ADC_HALF_BUFFER_SIZE (APP_ADC_DMA_BUFFER_SIZE / 2U)

#define APP_ADC_FLAG_HALF_COMPLETE (1UL << 0)
#define APP_ADC_FLAG_FULL_COMPLETE (1UL << 1)
#define APP_LINK_FLAG_TX_COMPLETE  (1UL << 2)
#define APP_LINK_FLAG_TX_ERROR     (1UL << 3)

#define APP_DISPLAY_TASK_PERIOD_MS 25U
#define APP_KEY_TASK_PERIOD_MS     10U
#define APP_PLOT_SAMPLE_PERIOD_MS  125U
#define APP_MONITOR_PERIOD_MS      500U
#define APP_BOOT_BANNER_HOLD_MS    1000U
#define APP_LINK_TRANSFER_TIMEOUT_MS 100U

#define APP_ADC_TASK_STACK_SIZE     (256U * 4U)
#define APP_LINK_TASK_STACK_SIZE    (512U * 4U)
#define APP_KEY_TASK_STACK_SIZE     (256U * 4U)
#define APP_DISPLAY_TASK_STACK_SIZE (512U * 4U)

#define APP_ADC_REFERENCE_MV           3300U
#define APP_ADC_SAMPLE_RATE_MIN_HZ     100U
#define APP_ADC_SAMPLE_RATE_MAX_HZ     2000U
#define APP_ADC_SAMPLE_RATE_STEP_HZ    100U
#define APP_ADC_SAMPLE_RATE_DEFAULT_HZ APP_ADC_SAMPLE_RATE_MIN_HZ
#define APP_TIM2_TRIGGER_CLOCK_HZ      1000000U
#define APP_PLOT_MV_PER_GRID           500U
#define APP_PLOT_MV_FULL_SCALE         (APP_PLOT_MV_PER_GRID * 7U)
#define APP_PLOT_SECONDS_PER_GRID      2U
#define APP_PLOT_GRID_X_PIXELS         16U
#define APP_PLOT_X_TICK_PIXELS         4U
#define APP_PLOT_Y_TICK_PIXELS         4U
#define APP_PLOT_PAGE_ALL              APP_ADC_CHANNEL_COUNT
#define APP_PLOT_PAGE_COUNT            (APP_ADC_CHANNEL_COUNT + 1U)
#define APP_LINK_PACKET_POOL_SIZE      4U
#define APP_LINK_PROTOCOL_MAGIC        0x314B4E4CUL
#define APP_LINK_PROTOCOL_VERSION      1U
#define APP_LINK_SAMPLE_BITS           12U
#define APP_LINK_PACKET_FLAG_HALF      0x0001U
#define APP_LINK_PACKET_FLAG_FULL      0x0002U

static volatile uint16_t app_adc_dma_buffer[APP_ADC_DMA_BUFFER_SIZE];

typedef struct
{
  uint32_t sequence;
  uint16_t average[APP_ADC_CHANNEL_COUNT];
  uint16_t minimum[APP_ADC_CHANNEL_COUNT];
  uint16_t maximum[APP_ADC_CHANNEL_COUNT];
} AppAdcStats;

typedef struct __attribute__((packed))
{
  uint32_t magic;
  uint16_t version;
  uint16_t header_bytes;
  uint32_t sequence;
  uint32_t tick_ms;
  uint32_t sample_rate_hz;
  uint16_t channel_count;
  uint16_t samples_per_channel;
  uint16_t bits_per_sample;
  uint16_t flags;
  uint32_t dropped_packets;
  uint32_t payload_bytes;
  uint32_t checksum;
} AppLinkPacketHeader;

#define APP_LINK_PACKET_HEADER_BYTES        ((uint16_t)sizeof(AppLinkPacketHeader))
#define APP_LINK_PACKET_PAYLOAD_BYTES       (APP_ADC_HALF_BUFFER_SIZE * sizeof(uint16_t))
#define APP_LINK_PACKET_TOTAL_BYTES         ((uint16_t)(APP_LINK_PACKET_HEADER_BYTES + APP_LINK_PACKET_PAYLOAD_BYTES))
#define APP_LINK_PACKET_WIRE_SUFFIX_BYTES   4U
#define APP_LINK_PACKET_WIRE_BYTES          ((uint16_t)(APP_LINK_PACKET_TOTAL_BYTES + APP_LINK_PACKET_WIRE_SUFFIX_BYTES))
#define APP_LINK_PACKET_SAMPLES_PER_CHANNEL (APP_ADC_HALF_BUFFER_SIZE / APP_ADC_CHANNEL_COUNT)

_Static_assert(sizeof(AppLinkPacketHeader) == 40U, "Unexpected link header size");
_Static_assert(APP_LINK_PACKET_TOTAL_BYTES <= 0xFFFFU, "SPI packet too large");
_Static_assert(APP_LINK_PACKET_WIRE_BYTES <= 0xFFFFU, "SPI wire packet too large");

static osThreadId_t app_adc_task_handle = NULL;
static osThreadId_t app_link_task_handle = NULL;
static osThreadId_t app_key_task_handle = NULL;
static osThreadId_t app_display_task_handle = NULL;
static osThreadId_t app_monitor_task_handle = NULL;
static osMessageQueueId_t app_key_queue = NULL;
static osMessageQueueId_t app_display_queue = NULL;
static osMessageQueueId_t app_link_free_queue = NULL;
static osMessageQueueId_t app_link_tx_queue = NULL;
static osMessageQueueId_t app_monitor_queue = NULL;
static uint32_t app_adc_sequence = 0U;
static uint32_t app_link_dropped_packets = 0U;
static uint32_t app_link_transfer_errors = 0U;
static uint32_t app_link_packets_sent = 0U;
static volatile uint32_t app_link_last_spi_error = HAL_SPI_ERROR_NONE;
static volatile uint32_t app_link_last_start_status = HAL_OK;

static uint16_t app_adc_latest_mv[APP_ADC_CHANNEL_COUNT];
static uint16_t app_adc_history[APP_ADC_CHANNEL_COUNT][COMP_OLED_WIDTH];
static uint8_t app_link_packet_pool[APP_LINK_PACKET_POOL_SIZE][APP_LINK_PACKET_WIRE_BYTES];
static uint8_t app_link_rx_buffer[APP_LINK_PACKET_WIRE_BYTES];
static uint16_t app_adc_history_count = 0U;
static uint8_t app_adc_latest_valid = 0U;
static uint8_t app_plot_page = 0U;
static volatile uint32_t app_adc_channel_sample_rate_hz = APP_ADC_SAMPLE_RATE_DEFAULT_HZ;

static const osThreadAttr_t app_adc_task_attributes = {
  .name = "adcTask",
  .stack_size = APP_ADC_TASK_STACK_SIZE,
  .priority = osPriorityHigh,
};

static const osThreadAttr_t app_link_task_attributes = {
  .name = "linkTask",
  .stack_size = APP_LINK_TASK_STACK_SIZE,
  .priority = osPriorityAboveNormal,
};

static const osThreadAttr_t app_key_task_attributes = {
  .name = "keyTask",
  .stack_size = APP_KEY_TASK_STACK_SIZE,
  .priority = osPriorityLow,
};

static const osThreadAttr_t app_display_task_attributes = {
  .name = "displayTask",
  .stack_size = APP_DISPLAY_TASK_STACK_SIZE,
  .priority = osPriorityBelowNormal,
};

static const osThreadAttr_t app_monitor_task_attributes = {
  .name = "monitorTask",
  .stack_size = 256U * 4U,
  .priority = osPriorityLow,
};

static const osMessageQueueAttr_t app_key_queue_attributes = {
  .name = "keyQueue",
};

static const osMessageQueueAttr_t app_display_queue_attributes = {
  .name = "displayQueue",
};

static const osMessageQueueAttr_t app_link_free_queue_attributes = {
  .name = "linkFreeQueue",
};

static const osMessageQueueAttr_t app_link_tx_queue_attributes = {
  .name = "linkTxQueue",
};

static const osMessageQueueAttr_t app_monitor_queue_attributes = {
  .name = "monitorQueue",
};

static void app_adc_task(void *argument);
static void app_link_task(void *argument);
static void app_key_task(void *argument);
static void app_display_task(void *argument);
static void app_monitor_task(void *argument);
static void app_compute_adc_stats(const volatile uint16_t *samples,
                                  uint32_t sample_count,
                                  AppAdcStats *stats);
static uint32_t app_link_checksum32(const uint8_t *data, uint32_t length);
static void app_release_link_packet(uint8_t packet_index);
static void app_queue_link_packet(const volatile uint16_t *samples,
                                  uint32_t sample_count,
                                  const AppAdcStats *stats,
                                  uint16_t flags);
static void app_publish_stats(const AppAdcStats *stats);
static void app_display_boot_banner(const char *line1, const char *line2);
static void app_log_stats(const AppAdcStats *stats);
static void app_require_status(uint8_t condition, const char *message);
static uint16_t app_adc_raw_to_mv(uint16_t raw_value);
static void app_update_latest_mv(const AppAdcStats *stats);
static void app_append_history_sample(void);
static uint8_t app_plot_voltage_to_y(uint16_t millivolts);
static void app_draw_plot_grid(void);
static void app_draw_channel_trace(uint32_t channel_index);
static void app_render_plot_page(void);
static void app_advance_plot_page(void);
static void app_log_plot_page(void);
static uint32_t app_get_adc_sample_rate_hz(void);
static uint32_t app_next_adc_sample_rate(uint32_t sample_rate_hz);
static void app_apply_adc_sample_rate(uint32_t sample_rate_hz);

static uint32_t app_get_adc_sample_rate_hz(void)
{
  return app_adc_channel_sample_rate_hz;
}

static uint32_t app_next_adc_sample_rate(uint32_t sample_rate_hz)
{
  uint32_t normalized_rate = sample_rate_hz;

  if (normalized_rate < APP_ADC_SAMPLE_RATE_MIN_HZ)
  {
    normalized_rate = APP_ADC_SAMPLE_RATE_MIN_HZ;
  }
  else if (normalized_rate > APP_ADC_SAMPLE_RATE_MAX_HZ)
  {
    normalized_rate = APP_ADC_SAMPLE_RATE_MAX_HZ;
  }

  if (normalized_rate >= APP_ADC_SAMPLE_RATE_MAX_HZ)
  {
    return APP_ADC_SAMPLE_RATE_MIN_HZ;
  }

  normalized_rate += APP_ADC_SAMPLE_RATE_STEP_HZ;
  if (normalized_rate > APP_ADC_SAMPLE_RATE_MAX_HZ)
  {
    normalized_rate = APP_ADC_SAMPLE_RATE_MIN_HZ;
  }

  return normalized_rate;
}

static void app_apply_adc_sample_rate(uint32_t sample_rate_hz)
{
  uint32_t normalized_rate = sample_rate_hz;
  uint32_t timer_enabled;
  uint32_t autoreload;

  if (normalized_rate < APP_ADC_SAMPLE_RATE_MIN_HZ)
  {
    normalized_rate = APP_ADC_SAMPLE_RATE_MIN_HZ;
  }
  else if (normalized_rate > APP_ADC_SAMPLE_RATE_MAX_HZ)
  {
    normalized_rate = APP_ADC_SAMPLE_RATE_MAX_HZ;
  }

  autoreload = (APP_TIM2_TRIGGER_CLOCK_HZ / normalized_rate) - 1U;

  taskENTER_CRITICAL();
  timer_enabled = ((htim2.Instance->CR1 & TIM_CR1_CEN) != 0U) ? 1U : 0U;

  if (timer_enabled != 0U)
  {
    __HAL_TIM_DISABLE(&htim2);
  }

  __HAL_TIM_SET_AUTORELOAD(&htim2, autoreload);
  __HAL_TIM_SET_COUNTER(&htim2, 0U);
  htim2.Init.Period = autoreload;
  app_adc_channel_sample_rate_hz = normalized_rate;

  if (timer_enabled != 0U)
  {
    __HAL_TIM_ENABLE(&htim2);
  }

  taskEXIT_CRITICAL();
}

static void app_display_boot_banner(const char *line1, const char *line2)
{
  CompOled_Clear();
  CompOled_DrawString(0U, 0U, "OLED READY");
  CompOled_DrawString(0U, 16U, line1);
  CompOled_DrawString(0U, 32U, line2);
  CompOled_Update();
}

static uint16_t app_adc_raw_to_mv(uint16_t raw_value)
{
  return (uint16_t)((((uint32_t)raw_value * APP_ADC_REFERENCE_MV) + 2047U) / 4095U);
}

static void app_update_latest_mv(const AppAdcStats *stats)
{
  if (stats == NULL)
  {
    return;
  }

  for (uint32_t channel = 0U; channel < APP_ADC_CHANNEL_COUNT; ++channel)
  {
    app_adc_latest_mv[channel] = app_adc_raw_to_mv(stats->average[channel]);
  }

  app_adc_latest_valid = 1U;
}

static void app_append_history_sample(void)
{
  for (uint32_t channel = 0U; channel < APP_ADC_CHANNEL_COUNT; ++channel)
  {
    uint16_t next_value = 0U;

    if (app_adc_latest_valid != 0U)
    {
      next_value = app_adc_latest_mv[channel];
    }
    else if (app_adc_history_count != 0U)
    {
      next_value = app_adc_history[channel][COMP_OLED_WIDTH - 1U];
    }

    (void)memmove(&app_adc_history[channel][0],
                  &app_adc_history[channel][1],
                  (COMP_OLED_WIDTH - 1U) * sizeof(app_adc_history[channel][0]));
    app_adc_history[channel][COMP_OLED_WIDTH - 1U] = next_value;
  }

  if (app_adc_history_count < COMP_OLED_WIDTH)
  {
    ++app_adc_history_count;
  }
}

static uint8_t app_plot_voltage_to_y(uint16_t millivolts)
{
  uint32_t clamped_mv = millivolts;
  uint32_t scaled;

  if (clamped_mv > APP_PLOT_MV_FULL_SCALE)
  {
    clamped_mv = APP_PLOT_MV_FULL_SCALE;
  }

  scaled = ((clamped_mv * (COMP_OLED_HEIGHT - 1U)) + (APP_PLOT_MV_FULL_SCALE / 2U)) /
           APP_PLOT_MV_FULL_SCALE;

  return (uint8_t)((COMP_OLED_HEIGHT - 1U) - scaled);
}

static void app_draw_plot_grid(void)
{
  const uint8_t x_axis_y = (uint8_t)(COMP_OLED_HEIGHT - 1U);

  CompOled_DrawLine(0, 0, 0, (int16_t)x_axis_y, 1U);
  CompOled_DrawLine(0, (int16_t)x_axis_y, (int16_t)(COMP_OLED_WIDTH - 1U), (int16_t)x_axis_y, 1U);

  for (uint32_t x = 0U; x < COMP_OLED_WIDTH; x += APP_PLOT_GRID_X_PIXELS)
  {
    uint8_t tick_top = (x_axis_y >= (APP_PLOT_X_TICK_PIXELS - 1U))
                         ? (uint8_t)(x_axis_y - (APP_PLOT_X_TICK_PIXELS - 1U))
                         : 0U;

    CompOled_DrawLine((int16_t)x, (int16_t)tick_top, (int16_t)x, (int16_t)x_axis_y, 1U);
  }

  for (uint32_t millivolts = 0U; millivolts <= APP_PLOT_MV_FULL_SCALE; millivolts += APP_PLOT_MV_PER_GRID)
  {
    uint8_t y = app_plot_voltage_to_y((uint16_t)millivolts);

    CompOled_DrawLine(0, (int16_t)y, (int16_t)(APP_PLOT_Y_TICK_PIXELS - 1U), (int16_t)y, 1U);
  }
}

static void app_draw_channel_trace(uint32_t channel_index)
{
  uint16_t start_index;

  if ((channel_index >= APP_ADC_CHANNEL_COUNT) || (app_adc_history_count == 0U))
  {
    return;
  }

  start_index = (uint16_t)(COMP_OLED_WIDTH - app_adc_history_count);

  if (app_adc_history_count == 1U)
  {
    CompOled_SetPixel((uint8_t)(COMP_OLED_WIDTH - 1U),
                      app_plot_voltage_to_y(app_adc_history[channel_index][COMP_OLED_WIDTH - 1U]),
                      1U);
    return;
  }

  for (uint16_t index = start_index + 1U; index < COMP_OLED_WIDTH; ++index)
  {
    uint8_t y0 = app_plot_voltage_to_y(app_adc_history[channel_index][index - 1U]);
    uint8_t y1 = app_plot_voltage_to_y(app_adc_history[channel_index][index]);

    CompOled_DrawLine((int16_t)(index - 1U), (int16_t)y0, (int16_t)index, (int16_t)y1, 1U);
  }
}

static void app_render_plot_page(void)
{
  char header[22];
  uint32_t sample_rate_hz = app_get_adc_sample_rate_hz();

  CompOled_Clear();
  app_draw_plot_grid();

  if (app_plot_page < APP_ADC_CHANNEL_COUNT)
  {
    uint32_t channel = app_plot_page;

    app_draw_channel_trace(channel);
    (void)snprintf(header,
                   sizeof(header),
                   "CH%lu %4uMV %4luHZ",
                   (unsigned long)(channel + 1U),
                   (unsigned int)app_adc_latest_mv[channel],
                   (unsigned long)sample_rate_hz);
  }
  else
  {
    for (uint32_t channel = 0U; channel < APP_ADC_CHANNEL_COUNT; ++channel)
    {
      app_draw_channel_trace(channel);
    }

    (void)snprintf(header, sizeof(header), "ALL 1-6 %4luHZ", (unsigned long)sample_rate_hz);
  }

  CompOled_DrawString(0U, 0U, header);
  CompOled_Update();
}

static void app_advance_plot_page(void)
{
  app_plot_page = (uint8_t)((app_plot_page + 1U) % APP_PLOT_PAGE_COUNT);
}

static void app_log_plot_page(void)
{
  if (app_plot_page < APP_ADC_CHANNEL_COUNT)
  {
    DrvUartLog_Printf("[KEY] page=CH%u\r\n", (unsigned int)(app_plot_page + 1U));
  }
  else
  {
    DrvUartLog_Printf("[KEY] page=ALL\r\n");
  }
}

static uint32_t app_link_checksum32(const uint8_t *data, uint32_t length)
{
  uint32_t hash = 2166136261UL;

  if (data == NULL)
  {
    return 0U;
  }

  for (uint32_t index = 0U; index < length; ++index)
  {
    hash ^= data[index];
    hash *= 16777619UL;
  }

  return hash;
}

static void app_release_link_packet(uint8_t packet_index)
{
  if (app_link_free_queue != NULL)
  {
    (void)osMessageQueuePut(app_link_free_queue, &packet_index, 0U, 0U);
  }
}

static void app_queue_link_packet(const volatile uint16_t *samples,
                                  uint32_t sample_count,
                                  const AppAdcStats *stats,
                                  uint16_t flags)
{
  uint8_t packet_index;
  uint8_t *wire_bytes;
  uint8_t *packet_bytes;
  AppLinkPacketHeader *header;
  uint16_t *payload_words;

  if ((samples == NULL) || (stats == NULL) || (sample_count != APP_ADC_HALF_BUFFER_SIZE) ||
      (app_link_free_queue == NULL) || (app_link_tx_queue == NULL))
  {
    return;
  }

  if (osMessageQueueGet(app_link_free_queue, &packet_index, NULL, 0U) != osOK)
  {
    ++app_link_dropped_packets;
    return;
  }

  wire_bytes = app_link_packet_pool[packet_index];
  memset(wire_bytes, 0, APP_LINK_PACKET_WIRE_BYTES);
  packet_bytes = wire_bytes;
  header = (AppLinkPacketHeader *)packet_bytes;
  payload_words = (uint16_t *)&packet_bytes[APP_LINK_PACKET_HEADER_BYTES];

  for (uint32_t sample_index = 0U; sample_index < sample_count; ++sample_index)
  {
    payload_words[sample_index] = (uint16_t)samples[sample_index];
  }

  header->magic = APP_LINK_PROTOCOL_MAGIC;
  header->version = APP_LINK_PROTOCOL_VERSION;
  header->header_bytes = APP_LINK_PACKET_HEADER_BYTES;
  header->sequence = stats->sequence;
  header->tick_ms = osKernelGetTickCount();
  header->sample_rate_hz = app_get_adc_sample_rate_hz();
  header->channel_count = APP_ADC_CHANNEL_COUNT;
  header->samples_per_channel = APP_LINK_PACKET_SAMPLES_PER_CHANNEL;
  header->bits_per_sample = APP_LINK_SAMPLE_BITS;
  header->flags = flags;
  header->dropped_packets = app_link_dropped_packets;
  header->payload_bytes = APP_LINK_PACKET_PAYLOAD_BYTES;
  header->checksum = 0U;
  header->checksum = app_link_checksum32(packet_bytes, APP_LINK_PACKET_TOTAL_BYTES);

  if (osMessageQueuePut(app_link_tx_queue, &packet_index, 0U, 0U) != osOK)
  {
    ++app_link_dropped_packets;
    app_release_link_packet(packet_index);
  }
}

static void app_compute_adc_stats(const volatile uint16_t *samples,
                                  uint32_t sample_count,
                                  AppAdcStats *stats)
{
  uint32_t accumulator[APP_ADC_CHANNEL_COUNT] = {0U};
  uint32_t frame_count;

  if ((samples == NULL) || (stats == NULL) || (sample_count == 0U))
  {
    return;
  }

  frame_count = sample_count / APP_ADC_CHANNEL_COUNT;
  if (frame_count == 0U)
  {
    return;
  }

  for (uint32_t channel = 0U; channel < APP_ADC_CHANNEL_COUNT; ++channel)
  {
    stats->minimum[channel] = 0xFFFFU;
    stats->maximum[channel] = 0U;
  }

  for (uint32_t index = 0U; index < sample_count; index += APP_ADC_CHANNEL_COUNT)
  {
    for (uint32_t channel = 0U; channel < APP_ADC_CHANNEL_COUNT; ++channel)
    {
      uint16_t value = samples[index + channel];

      accumulator[channel] += value;

      if (value < stats->minimum[channel])
      {
        stats->minimum[channel] = value;
      }

      if (value > stats->maximum[channel])
      {
        stats->maximum[channel] = value;
      }
    }
  }

  for (uint32_t channel = 0U; channel < APP_ADC_CHANNEL_COUNT; ++channel)
  {
    stats->average[channel] = (uint16_t)(accumulator[channel] / frame_count);
  }

  stats->sequence = ++app_adc_sequence;
}

static void app_publish_stats(const AppAdcStats *stats)
{
  AppAdcStats discarded_stats;

  if (stats == NULL)
  {
    return;
  }

  if (app_display_queue != NULL)
  {
    if (osMessageQueuePut(app_display_queue, stats, 0U, 0U) != osOK)
    {
      (void)osMessageQueueGet(app_display_queue, &discarded_stats, NULL, 0U);
      (void)osMessageQueuePut(app_display_queue, stats, 0U, 0U);
    }
  }

  if (app_monitor_queue != NULL)
  {
    if (osMessageQueuePut(app_monitor_queue, stats, 0U, 0U) != osOK)
    {
      (void)osMessageQueueGet(app_monitor_queue, &discarded_stats, NULL, 0U);
      (void)osMessageQueuePut(app_monitor_queue, stats, 0U, 0U);
    }
  }
}

static void app_log_stats(const AppAdcStats *stats)
{
  if (stats == NULL)
  {
    return;
  }

  DrvUartLog_Printf("[MON] seq=%lu avg=%u,%u,%u,%u,%u,%u\r\n",
                    (unsigned long)stats->sequence,
                    (unsigned int)stats->average[0],
                    (unsigned int)stats->average[1],
                    (unsigned int)stats->average[2],
                    (unsigned int)stats->average[3],
                    (unsigned int)stats->average[4],
                    (unsigned int)stats->average[5]);

  DrvUartLog_Printf("[MON] link sent=%lu drop=%lu err=%lu queued=%lu\r\n",
                    (unsigned long)app_link_packets_sent,
                    (unsigned long)app_link_dropped_packets,
                    (unsigned long)app_link_transfer_errors,
                    (unsigned long)((app_link_tx_queue != NULL) ? osMessageQueueGetCount(app_link_tx_queue) : 0U));

  DrvUartLog_Printf("[MON] link spi status=%lu last_err=0x%08lX\r\n",
                    (unsigned long)app_link_last_start_status,
                    (unsigned long)app_link_last_spi_error);

  DrvUartLog_Printf("[MON] stack adc=%lu link=%lu disp=%lu mon=%lu words\r\n",
                    (unsigned long)uxTaskGetStackHighWaterMark((TaskHandle_t)app_adc_task_handle),
                    (unsigned long)uxTaskGetStackHighWaterMark((TaskHandle_t)app_link_task_handle),
                    (unsigned long)uxTaskGetStackHighWaterMark((TaskHandle_t)app_display_task_handle),
                    (unsigned long)uxTaskGetStackHighWaterMark((TaskHandle_t)app_monitor_task_handle));
}

static void app_require_status(uint8_t condition, const char *message)
{
  if (condition != 0U)
  {
    return;
  }

  if (message != NULL)
  {
    DrvUartLog_Printf("%s\r\n", message);
  }

  Error_Handler();
}

static void app_adc_task(void *argument)
{
  uint32_t flags;
  AppAdcStats stats;

  (void)argument;

  if (HAL_ADC_Start_DMA(&hadc1, (uint32_t *)app_adc_dma_buffer, APP_ADC_DMA_BUFFER_SIZE) != HAL_OK)
  {
    DrvUartLog_Printf("[ADC] start dma failed\r\n");
    Error_Handler();
  }

  app_apply_adc_sample_rate(app_get_adc_sample_rate_hz());
  __HAL_TIM_SET_COUNTER(&htim2, 0U);
  if (HAL_TIM_Base_Start(&htim2) != HAL_OK)
  {
    DrvUartLog_Printf("[ADC] tim2 start failed\r\n");
    Error_Handler();
  }

  DrvUartLog_Printf("[ADC] scan dma armed, trigger=TIM2 %luHz, buffer=%u samples\r\n",
                    (unsigned long)app_get_adc_sample_rate_hz(),
                    (unsigned int)APP_ADC_DMA_BUFFER_SIZE);

  for (;;)
  {
    flags = osThreadFlagsWait(APP_ADC_FLAG_HALF_COMPLETE | APP_ADC_FLAG_FULL_COMPLETE,
                              osFlagsWaitAny,
                              osWaitForever);

    if ((flags & osFlagsError) != 0U)
    {
      continue;
    }

    if ((flags & APP_ADC_FLAG_HALF_COMPLETE) != 0U)
    {
      app_compute_adc_stats(app_adc_dma_buffer, APP_ADC_HALF_BUFFER_SIZE, &stats);
      app_publish_stats(&stats);
      app_queue_link_packet(app_adc_dma_buffer,
                            APP_ADC_HALF_BUFFER_SIZE,
                            &stats,
                            APP_LINK_PACKET_FLAG_HALF);
    }

    if ((flags & APP_ADC_FLAG_FULL_COMPLETE) != 0U)
    {
      app_compute_adc_stats(&app_adc_dma_buffer[APP_ADC_HALF_BUFFER_SIZE],
                            APP_ADC_HALF_BUFFER_SIZE,
                            &stats);
      app_publish_stats(&stats);
      app_queue_link_packet(&app_adc_dma_buffer[APP_ADC_HALF_BUFFER_SIZE],
                            APP_ADC_HALF_BUFFER_SIZE,
                            &stats,
                            APP_LINK_PACKET_FLAG_FULL);
    }
  }
}

static void app_link_task(void *argument)
{
  uint8_t packet_index;
  uint32_t flags;

  (void)argument;

  DrvEspLink_Init();
  DrvUartLog_Printf("[LINK] SPI2 slave PB12/PB13/PB14/PB15 RDY=PB5\r\n");
  DrvUartLog_Printf("[LINK] pkt=%uB wire=%uB payload=%uB %luHz/ch\r\n",
                    (unsigned int)APP_LINK_PACKET_TOTAL_BYTES,
                    (unsigned int)APP_LINK_PACKET_WIRE_BYTES,
                    (unsigned int)APP_LINK_PACKET_PAYLOAD_BYTES,
                    (unsigned long)app_get_adc_sample_rate_hz());

  for (;;)
  {
    if (osMessageQueueGet(app_link_tx_queue, &packet_index, NULL, osWaitForever) != osOK)
    {
      continue;
    }

    (void)osThreadFlagsClear(APP_LINK_FLAG_TX_COMPLETE | APP_LINK_FLAG_TX_ERROR);

    app_link_last_spi_error = HAL_SPI_ERROR_NONE;
    app_link_last_start_status = (uint32_t)DrvEspLink_TransmitPacket(app_link_packet_pool[packet_index],
                                                                     app_link_rx_buffer,
                                                                     APP_LINK_PACKET_WIRE_BYTES);

    if (app_link_last_start_status != (uint32_t)HAL_OK)
    {
      ++app_link_transfer_errors;
      DrvEspLink_Reset();
      app_release_link_packet(packet_index);
      continue;
    }

    flags = osThreadFlagsWait(APP_LINK_FLAG_TX_COMPLETE | APP_LINK_FLAG_TX_ERROR,
                              osFlagsWaitAny,
                              APP_LINK_TRANSFER_TIMEOUT_MS);

    if ((flags & osFlagsError) != 0U)
    {
      ++app_link_transfer_errors;
      DrvEspLink_Reset();
    }
    else if ((flags & APP_LINK_FLAG_TX_ERROR) != 0U)
    {
      ++app_link_transfer_errors;
      DrvEspLink_Reset();
    }
    else
    {
      ++app_link_packets_sent;
    }

    app_release_link_packet(packet_index);
  }
}

static void app_key_task(void *argument)
{
  uint32_t next_wakeup;

  (void)argument;

  DrvKeys_Init();
  next_wakeup = osKernelGetTickCount();

  for (;;)
  {
    uint32_t key_events = DrvKeys_PollEvents();

    key_events &= (DRV_KEY_EVENT_KEY1 | DRV_KEY_EVENT_KEY2);

    if ((key_events != 0U) && (app_key_queue != NULL))
    {
      if (osMessageQueuePut(app_key_queue, &key_events, 0U, 0U) != osOK)
      {
        uint32_t discarded_event;

        (void)osMessageQueueGet(app_key_queue, &discarded_event, NULL, 0U);
        (void)osMessageQueuePut(app_key_queue, &key_events, 0U, 0U);
      }
    }

    next_wakeup += APP_KEY_TASK_PERIOD_MS;
    (void)osDelayUntil(next_wakeup);
  }
}

static void app_display_task(void *argument)
{
  AppAdcStats latest_stats;
  uint32_t key_events;
  uint32_t next_wakeup;
  uint32_t next_sample_tick;
  uint8_t render_needed = 1U;

  (void)argument;

  osDelay(APP_BOOT_BANNER_HOLD_MS);

  next_wakeup = osKernelGetTickCount();
  next_sample_tick = next_wakeup + APP_PLOT_SAMPLE_PERIOD_MS;

  for (;;)
  {
    uint32_t now;

    while (osMessageQueueGet(app_display_queue, &latest_stats, NULL, 0U) == osOK)
    {
      app_update_latest_mv(&latest_stats);
    }

    while ((app_key_queue != NULL) && (osMessageQueueGet(app_key_queue, &key_events, NULL, 0U) == osOK))
    {
      if ((key_events & DRV_KEY_EVENT_KEY1) != 0U)
      {
        uint32_t next_rate = app_next_adc_sample_rate(app_get_adc_sample_rate_hz());

        app_apply_adc_sample_rate(next_rate);
        DrvUartLog_Printf("[KEY] rate=%luHz/ch\r\n", (unsigned long)next_rate);
        render_needed = 1U;
      }

      if ((key_events & DRV_KEY_EVENT_KEY2) != 0U)
      {
        app_advance_plot_page();
        app_log_plot_page();
        render_needed = 1U;
      }
    }

    now = osKernelGetTickCount();
    while ((int32_t)(now - next_sample_tick) >= 0)
    {
      app_append_history_sample();
      next_sample_tick += APP_PLOT_SAMPLE_PERIOD_MS;
      render_needed = 1U;
    }

    if (render_needed != 0U)
    {
      app_render_plot_page();
      render_needed = 0U;
    }

    next_wakeup += APP_DISPLAY_TASK_PERIOD_MS;
    (void)osDelayUntil(next_wakeup);
  }
}

void App_Init(void)
{
  DrvUartLog_Init();
  DrvKeys_Init();
  DrvEspLink_Init();
  DrvUartLog_Printf("\r\n[BOOT] app init\r\n");
  DrvUartLog_Printf("[UART] USART1 PB6/PB7 115200 8N1\r\n");
  DrvUartLog_Printf("[RTOS] CubeMX FreeRTOS CMSIS-V2 enabled\r\n");
  DrvUartLog_Printf("[RTOS] adc=High link=AboveNormal display=BelowNormal monitor=Low slice=1ms\r\n");
  DrvUartLog_Printf("[RTOS] irq->thread flags, stats->single-slot queues\r\n");
  DrvUartLog_Printf("[KEY] KEY0=PE4 KEY1=PE3(rate) KEY2=PE2(page)\r\n");
  DrvUartLog_Printf("[OLED] SPI3 SCK=PC10 MOSI=PC12 CS=PG11 RST=PG13 DC=PG15\r\n");
  DrvUartLog_Printf("[OLED] plot 2S/GRID 500MV/GRID\r\n");
  DrvUartLog_Printf("[OLED] init demo-compatible\r\n");
  DrvUartLog_Printf("[ADC] CH1..CH6=PA0/PA1/PA4/PA5/PA6/PA7 TIM2=%luHz %luHz/ch\r\n",
                    (unsigned long)app_get_adc_sample_rate_hz(),
                    (unsigned long)app_get_adc_sample_rate_hz());
  DrvUartLog_Printf("[ADC] KEY1 cycles %lu..%luHz/ch step=%luHz\r\n",
                    (unsigned long)APP_ADC_SAMPLE_RATE_MIN_HZ,
                    (unsigned long)APP_ADC_SAMPLE_RATE_MAX_HZ,
                    (unsigned long)APP_ADC_SAMPLE_RATE_STEP_HZ);
  CompOled_Init();
  app_display_boot_banner("ADC CURVE", "K1 RATE K2 NEXT");
}

void App_RtosInit(void)
{
  app_key_queue = osMessageQueueNew(2U,
                                    sizeof(uint32_t),
                                    &app_key_queue_attributes);
  app_display_queue = osMessageQueueNew(1U,
                                        sizeof(AppAdcStats),
                                        &app_display_queue_attributes);
  app_link_free_queue = osMessageQueueNew(APP_LINK_PACKET_POOL_SIZE,
                                          sizeof(uint8_t),
                                          &app_link_free_queue_attributes);
  app_link_tx_queue = osMessageQueueNew(APP_LINK_PACKET_POOL_SIZE,
                                        sizeof(uint8_t),
                                        &app_link_tx_queue_attributes);
  app_monitor_queue = osMessageQueueNew(1U,
                                        sizeof(AppAdcStats),
                                        &app_monitor_queue_attributes);

  app_require_status((app_key_queue != NULL) ? 1U : 0U,
                     "[RTOS] key queue create failed");
  app_require_status((app_display_queue != NULL) ? 1U : 0U,
                     "[RTOS] display queue create failed");
  app_require_status((app_link_free_queue != NULL) ? 1U : 0U,
                     "[RTOS] link free queue create failed");
  app_require_status((app_link_tx_queue != NULL) ? 1U : 0U,
                     "[RTOS] link tx queue create failed");
  app_require_status((app_monitor_queue != NULL) ? 1U : 0U,
                     "[RTOS] monitor queue create failed");

  for (uint8_t packet_index = 0U; packet_index < APP_LINK_PACKET_POOL_SIZE; ++packet_index)
  {
    app_require_status((osMessageQueuePut(app_link_free_queue, &packet_index, 0U, 0U) == osOK) ? 1U : 0U,
                       "[RTOS] link free queue seed failed");
  }

  app_adc_task_handle = osThreadNew(app_adc_task, NULL, &app_adc_task_attributes);
  app_link_task_handle = osThreadNew(app_link_task, NULL, &app_link_task_attributes);
  app_key_task_handle = osThreadNew(app_key_task, NULL, &app_key_task_attributes);
  app_display_task_handle = osThreadNew(app_display_task, NULL, &app_display_task_attributes);
  app_monitor_task_handle = osThreadNew(app_monitor_task, NULL, &app_monitor_task_attributes);

  app_require_status((app_adc_task_handle != NULL) ? 1U : 0U,
                     "[RTOS] adc task create failed");
  app_require_status((app_link_task_handle != NULL) ? 1U : 0U,
                     "[RTOS] link task create failed");
  app_require_status((app_key_task_handle != NULL) ? 1U : 0U,
                     "[RTOS] key task create failed");
  app_require_status((app_display_task_handle != NULL) ? 1U : 0U,
                     "[RTOS] display task create failed");
  app_require_status((app_monitor_task_handle != NULL) ? 1U : 0U,
                     "[RTOS] monitor task create failed");

  DrvUartLog_Printf("[RTOS] tasks created\r\n");
}

static void app_monitor_task(void *argument)
{
  AppAdcStats latest_stats;
  uint8_t has_stats = 0U;
  uint32_t next_wakeup;

  (void)argument;

  DrvUartLog_Printf("[RTOS] monitor task online\r\n");

  next_wakeup = osKernelGetTickCount();

  for (;;)
  {
    if (osMessageQueueGet(app_monitor_queue, &latest_stats, NULL, 0U) == osOK)
    {
      has_stats = 1U;
    }

    if (has_stats != 0U)
    {
      app_log_stats(&latest_stats);
    }
    else
    {
      DrvUartLog_Printf("[MON] waiting adc data\r\n");
    }

    next_wakeup += APP_MONITOR_PERIOD_MS;
    (void)osDelayUntil(next_wakeup);
  }
}

void App_Run(void)
{
  /* FreeRTOS scheduler takes over after osKernelStart(). */
}

void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *hadc)
{
  if ((hadc->Instance == ADC1) && (app_adc_task_handle != NULL))
  {
    (void)osThreadFlagsSet(app_adc_task_handle, APP_ADC_FLAG_HALF_COMPLETE);
  }
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
  if ((hadc->Instance == ADC1) && (app_adc_task_handle != NULL))
  {
    (void)osThreadFlagsSet(app_adc_task_handle, APP_ADC_FLAG_FULL_COMPLETE);
  }
}

void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
  if ((hspi->Instance == SPI2) && (app_link_task_handle != NULL))
  {
    DrvEspLink_SetReady(0U);
    (void)osThreadFlagsSet(app_link_task_handle, APP_LINK_FLAG_TX_COMPLETE);
  }
}

void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
  if ((hspi->Instance == SPI2) && (app_link_task_handle != NULL))
  {
    app_link_last_spi_error = HAL_SPI_GetError(hspi);
    DrvEspLink_SetReady(0U);
    (void)osThreadFlagsSet(app_link_task_handle, APP_LINK_FLAG_TX_ERROR);
  }
}

void vApplicationMallocFailedHook(void)
{
  DrvUartLog_Printf("[RTOS] malloc failed\r\n");
  Error_Handler();
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *task_name)
{
  (void)task;
  DrvUartLog_Printf("[RTOS] stack overflow %s\r\n",
                    (task_name != NULL) ? task_name : "unknown");
  Error_Handler();
}
