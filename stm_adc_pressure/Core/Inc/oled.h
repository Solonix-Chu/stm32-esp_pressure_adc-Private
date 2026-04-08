/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    oled.h
  * @brief   Simple OLED driver for the onboard display.
  ******************************************************************************
  * @attention
  *
  * The board routes the OLED serial pins to regular GPIOs instead of an STM32
  * SPI peripheral, so this driver uses software SPI.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
#ifndef __OLED_H__
#define __OLED_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

#define OLED_WIDTH          128U
#define OLED_HEIGHT         64U
#define OLED_PAGE_COUNT     (OLED_HEIGHT / 8U)
#define OLED_BUFFER_SIZE    (OLED_WIDTH * OLED_PAGE_COUNT)

/* Set to 0 if the module is actually SSD1306-compatible instead of SH1106. */
#ifndef OLED_CONTROLLER_SH1106
#define OLED_CONTROLLER_SH1106 1U
#endif

void OLED_Init(void);
void OLED_Clear(void);
void OLED_Update(void);
void OLED_SetPixel(uint8_t x, uint8_t y, uint8_t on);
void OLED_DrawChar(uint8_t x, uint8_t y, char ch);
void OLED_DrawString(uint8_t x, uint8_t y, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* __OLED_H__ */
