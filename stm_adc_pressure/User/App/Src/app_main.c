#include "app_main.h"

#include "adc.h"
#include "comp_oled.h"
#include "drv_uart_log.h"

#define APP_ADC_CHANNEL_COUNT  6U
#define APP_ADC_DMA_FRAME_COUNT 64U
#define APP_ADC_DMA_BUFFER_SIZE (APP_ADC_CHANNEL_COUNT * APP_ADC_DMA_FRAME_COUNT)

static volatile uint16_t app_adc_dma_buffer[APP_ADC_DMA_BUFFER_SIZE];
static volatile uint8_t app_adc_dma_half_complete = 0U;
static volatile uint8_t app_adc_dma_full_complete = 0U;

static void app_display_boot_banner(const char *line1, const char *line2)
{
  CompOled_Clear();
  CompOled_DrawString(0U, 0U, "OLED READY");
  CompOled_DrawString(0U, 16U, line1);
  CompOled_DrawString(0U, 32U, line2);
  CompOled_Update();
}

void App_Init(void)
{
  DrvUartLog_Init();
  DrvUartLog_Printf("\r\n[BOOT] app init\r\n");
  DrvUartLog_Printf("[UART] USART1 PB6/PB7 115200 8N1\r\n");
  DrvUartLog_Printf("[OLED] init demo-compatible\r\n");
  CompOled_Init();
  app_display_boot_banner("ADC1 SCAN DMA", "PA1-PA6");

  if (HAL_ADC_Start_DMA(&hadc1, (uint32_t *)app_adc_dma_buffer, APP_ADC_DMA_BUFFER_SIZE) != HAL_OK)
  {
    DrvUartLog_Printf("[ADC] start dma failed\r\n");
    Error_Handler();
  }

  DrvUartLog_Printf("[ADC] scan dma started, buffer=%u samples\r\n",
                    (unsigned int)APP_ADC_DMA_BUFFER_SIZE);
}

void App_Run(void)
{
  if (app_adc_dma_half_complete != 0U)
  {
    app_adc_dma_half_complete = 0U;
  }

  if (app_adc_dma_full_complete != 0U)
  {
    app_adc_dma_full_complete = 0U;
  }
}

void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *hadc)
{
  if (hadc->Instance == ADC1)
  {
    app_adc_dma_half_complete = 1U;
  }
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
  if (hadc->Instance == ADC1)
  {
    app_adc_dma_full_complete = 1U;
  }
}
