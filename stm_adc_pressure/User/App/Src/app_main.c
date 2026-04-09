#include "app_main.h"

#include "adc.h"
#include "cmsis_os2.h"
#include "comp_oled.h"
#include "drv_uart_log.h"

#include "FreeRTOS.h"
#include "task.h"

#include <stdio.h>

#define APP_ADC_CHANNEL_COUNT  6U
#define APP_ADC_DMA_FRAME_COUNT 64U
#define APP_ADC_DMA_BUFFER_SIZE (APP_ADC_CHANNEL_COUNT * APP_ADC_DMA_FRAME_COUNT)
#define APP_ADC_HALF_BUFFER_SIZE (APP_ADC_DMA_BUFFER_SIZE / 2U)

#define APP_ADC_FLAG_HALF_COMPLETE (1UL << 0)
#define APP_ADC_FLAG_FULL_COMPLETE (1UL << 1)

#define APP_DISPLAY_PERIOD_MS 100U
#define APP_MONITOR_PERIOD_MS 500U
#define APP_BOOT_BANNER_HOLD_MS 1000U

#define APP_ADC_TASK_STACK_SIZE     (256U * 4U)
#define APP_DISPLAY_TASK_STACK_SIZE (384U * 4U)

static volatile uint16_t app_adc_dma_buffer[APP_ADC_DMA_BUFFER_SIZE];

typedef struct
{
  uint32_t sequence;
  uint16_t average[APP_ADC_CHANNEL_COUNT];
  uint16_t minimum[APP_ADC_CHANNEL_COUNT];
  uint16_t maximum[APP_ADC_CHANNEL_COUNT];
} AppAdcStats;

static osThreadId_t app_adc_task_handle = NULL;
static osThreadId_t app_display_task_handle = NULL;
static osThreadId_t app_monitor_task_handle = NULL;
static osMessageQueueId_t app_display_queue = NULL;
static osMessageQueueId_t app_monitor_queue = NULL;
static uint32_t app_adc_sequence = 0U;

static const osThreadAttr_t app_adc_task_attributes = {
  .name = "adcTask",
  .stack_size = APP_ADC_TASK_STACK_SIZE,
  .priority = osPriorityHigh,
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

static const osMessageQueueAttr_t app_display_queue_attributes = {
  .name = "displayQueue",
};

static const osMessageQueueAttr_t app_monitor_queue_attributes = {
  .name = "monitorQueue",
};

static void app_adc_task(void *argument);
static void app_display_task(void *argument);
static void app_monitor_task(void *argument);
static void app_compute_adc_stats(const volatile uint16_t *samples,
                                  uint32_t sample_count,
                                  AppAdcStats *stats);
static void app_publish_stats(const AppAdcStats *stats);
static void app_display_boot_banner(const char *line1, const char *line2);
static void app_display_stats(const AppAdcStats *stats);
static void app_log_stats(const AppAdcStats *stats);
static void app_require_status(uint8_t condition, const char *message);

static void app_display_boot_banner(const char *line1, const char *line2)
{
  CompOled_Clear();
  CompOled_DrawString(0U, 0U, "OLED READY");
  CompOled_DrawString(0U, 16U, line1);
  CompOled_DrawString(0U, 32U, line2);
  CompOled_Update();
}

static void app_display_stats(const AppAdcStats *stats)
{
  char line[16];

  if (stats == NULL)
  {
    return;
  }

  CompOled_Clear();

  for (uint32_t channel = 0U; channel < APP_ADC_CHANNEL_COUNT; ++channel)
  {
    (void)snprintf(line,
                   sizeof(line),
                   "C%lu %4u",
                   (unsigned long)(channel + 1U),
                   (unsigned int)stats->average[channel]);
    CompOled_DrawString(0U, (uint8_t)(channel * 10U), line);
  }

  CompOled_Update();
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

  DrvUartLog_Printf("[MON] stack adc=%lu disp=%lu mon=%lu words\r\n",
                    (unsigned long)uxTaskGetStackHighWaterMark((TaskHandle_t)app_adc_task_handle),
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

  DrvUartLog_Printf("[ADC] scan dma started, buffer=%u samples\r\n",
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
    }

    if ((flags & APP_ADC_FLAG_FULL_COMPLETE) != 0U)
    {
      app_compute_adc_stats(&app_adc_dma_buffer[APP_ADC_HALF_BUFFER_SIZE],
                            APP_ADC_HALF_BUFFER_SIZE,
                            &stats);
      app_publish_stats(&stats);
    }
  }
}

static void app_display_task(void *argument)
{
  AppAdcStats latest_stats;
  uint8_t has_stats = 0U;
  uint32_t next_wakeup;

  (void)argument;

  osDelay(APP_BOOT_BANNER_HOLD_MS);
  next_wakeup = osKernelGetTickCount();

  for (;;)
  {
    if (osMessageQueueGet(app_display_queue, &latest_stats, NULL, 0U) == osOK)
    {
      has_stats = 1U;
    }

    if (has_stats != 0U)
    {
      app_display_stats(&latest_stats);
    }

    next_wakeup += APP_DISPLAY_PERIOD_MS;
    (void)osDelayUntil(next_wakeup);
  }
}

void App_Init(void)
{
  DrvUartLog_Init();
  DrvUartLog_Printf("\r\n[BOOT] app init\r\n");
  DrvUartLog_Printf("[UART] USART1 PB6/PB7 115200 8N1\r\n");
  DrvUartLog_Printf("[RTOS] CubeMX FreeRTOS CMSIS-V2 enabled\r\n");
  DrvUartLog_Printf("[RTOS] adc=High display=BelowNormal monitor=Low slice=1ms\r\n");
  DrvUartLog_Printf("[RTOS] irq->thread flags, stats->single-slot queues\r\n");
  DrvUartLog_Printf("[OLED] init demo-compatible\r\n");
  CompOled_Init();
  app_display_boot_banner("FREERTOS READY", "WAIT DATA");
}

void App_RtosInit(void)
{
  app_display_queue = osMessageQueueNew(1U,
                                        sizeof(AppAdcStats),
                                        &app_display_queue_attributes);
  app_monitor_queue = osMessageQueueNew(1U,
                                        sizeof(AppAdcStats),
                                        &app_monitor_queue_attributes);

  app_require_status((app_display_queue != NULL) ? 1U : 0U,
                     "[RTOS] display queue create failed");
  app_require_status((app_monitor_queue != NULL) ? 1U : 0U,
                     "[RTOS] monitor queue create failed");

  app_adc_task_handle = osThreadNew(app_adc_task, NULL, &app_adc_task_attributes);
  app_display_task_handle = osThreadNew(app_display_task, NULL, &app_display_task_attributes);
  app_monitor_task_handle = osThreadNew(app_monitor_task, NULL, &app_monitor_task_attributes);

  app_require_status((app_adc_task_handle != NULL) ? 1U : 0U,
                     "[RTOS] adc task create failed");
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
