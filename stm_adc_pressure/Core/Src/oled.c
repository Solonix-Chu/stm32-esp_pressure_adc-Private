/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    oled.c
  * @brief   Onboard OLED software SPI driver.
  ******************************************************************************
  */
/* USER CODE END Header */
#include "oled.h"

#include <stddef.h>
#include <string.h>

#if OLED_CONTROLLER_SH1106
#define OLED_COLUMN_OFFSET 2U
#else
#define OLED_COLUMN_OFFSET 0U
#endif

static uint8_t oled_buffer[OLED_BUFFER_SIZE];

static inline void oled_clk_low(void)
{
  OLED_CLK_GPIO_Port->BSRR = (uint32_t)OLED_CLK_Pin << 16U;
}

static inline void oled_clk_high(void)
{
  OLED_CLK_GPIO_Port->BSRR = OLED_CLK_Pin;
}

static inline void oled_mosi_low(void)
{
  OLED_MOSI_GPIO_Port->BSRR = (uint32_t)OLED_MOSI_Pin << 16U;
}

static inline void oled_mosi_high(void)
{
  OLED_MOSI_GPIO_Port->BSRR = OLED_MOSI_Pin;
}

static inline void oled_dc_command(void)
{
  OLED_DC_GPIO_Port->BSRR = (uint32_t)OLED_DC_Pin << 16U;
}

static inline void oled_dc_data(void)
{
  OLED_DC_GPIO_Port->BSRR = OLED_DC_Pin;
}

static inline void oled_cs1_select(void)
{
  OLED_CS1_GPIO_Port->BSRR = (uint32_t)OLED_CS1_Pin << 16U;
}

static inline void oled_cs1_deselect(void)
{
  OLED_CS1_GPIO_Port->BSRR = OLED_CS1_Pin;
}

static inline void oled_cs2_deselect(void)
{
  OLED_CS2_GPIO_Port->BSRR = OLED_CS2_Pin;
}

static inline void oled_bus_delay(void)
{
  __NOP();
  __NOP();
  __NOP();
}

static const uint8_t *oled_get_glyph(char ch)
{
  static const uint8_t digits[10][5] = {
    {0x3EU, 0x51U, 0x49U, 0x45U, 0x3EU},
    {0x00U, 0x42U, 0x7FU, 0x40U, 0x00U},
    {0x42U, 0x61U, 0x51U, 0x49U, 0x46U},
    {0x21U, 0x41U, 0x45U, 0x4BU, 0x31U},
    {0x18U, 0x14U, 0x12U, 0x7FU, 0x10U},
    {0x27U, 0x45U, 0x45U, 0x45U, 0x39U},
    {0x3CU, 0x4AU, 0x49U, 0x49U, 0x30U},
    {0x01U, 0x71U, 0x09U, 0x05U, 0x03U},
    {0x36U, 0x49U, 0x49U, 0x49U, 0x36U},
    {0x06U, 0x49U, 0x49U, 0x29U, 0x1EU}
  };
  static const uint8_t letters[26][5] = {
    {0x7EU, 0x11U, 0x11U, 0x11U, 0x7EU},
    {0x7FU, 0x49U, 0x49U, 0x49U, 0x36U},
    {0x3EU, 0x41U, 0x41U, 0x41U, 0x22U},
    {0x7FU, 0x41U, 0x41U, 0x22U, 0x1CU},
    {0x7FU, 0x49U, 0x49U, 0x49U, 0x41U},
    {0x7FU, 0x09U, 0x09U, 0x09U, 0x01U},
    {0x3EU, 0x41U, 0x49U, 0x49U, 0x7AU},
    {0x7FU, 0x08U, 0x08U, 0x08U, 0x7FU},
    {0x00U, 0x41U, 0x7FU, 0x41U, 0x00U},
    {0x20U, 0x40U, 0x41U, 0x3FU, 0x01U},
    {0x7FU, 0x08U, 0x14U, 0x22U, 0x41U},
    {0x7FU, 0x40U, 0x40U, 0x40U, 0x40U},
    {0x7FU, 0x02U, 0x0CU, 0x02U, 0x7FU},
    {0x7FU, 0x04U, 0x08U, 0x10U, 0x7FU},
    {0x3EU, 0x41U, 0x41U, 0x41U, 0x3EU},
    {0x7FU, 0x09U, 0x09U, 0x09U, 0x06U},
    {0x3EU, 0x41U, 0x51U, 0x21U, 0x5EU},
    {0x7FU, 0x09U, 0x19U, 0x29U, 0x46U},
    {0x46U, 0x49U, 0x49U, 0x49U, 0x31U},
    {0x01U, 0x01U, 0x7FU, 0x01U, 0x01U},
    {0x3FU, 0x40U, 0x40U, 0x40U, 0x3FU},
    {0x1FU, 0x20U, 0x40U, 0x20U, 0x1FU},
    {0x3FU, 0x40U, 0x38U, 0x40U, 0x3FU},
    {0x63U, 0x14U, 0x08U, 0x14U, 0x63U},
    {0x07U, 0x08U, 0x70U, 0x08U, 0x07U},
    {0x61U, 0x51U, 0x49U, 0x45U, 0x43U}
  };
  static const uint8_t blank[5] = {0x00U, 0x00U, 0x00U, 0x00U, 0x00U};
  static const uint8_t dash[5] = {0x08U, 0x08U, 0x08U, 0x08U, 0x08U};
  static const uint8_t colon[5] = {0x00U, 0x36U, 0x36U, 0x00U, 0x00U};
  static const uint8_t dot[5] = {0x00U, 0x60U, 0x60U, 0x00U, 0x00U};
  static const uint8_t slash[5] = {0x20U, 0x10U, 0x08U, 0x04U, 0x02U};

  if ((ch >= 'a') && (ch <= 'z'))
  {
    ch = (char)(ch - ('a' - 'A'));
  }

  if ((ch >= '0') && (ch <= '9'))
  {
    return digits[(uint8_t)(ch - '0')];
  }

  if ((ch >= 'A') && (ch <= 'Z'))
  {
    return letters[(uint8_t)(ch - 'A')];
  }

  switch (ch)
  {
    case '-':
      return dash;
    case ':':
      return colon;
    case '.':
      return dot;
    case '/':
      return slash;
    case ' ':
    default:
      return blank;
  }
}

static void oled_write_byte(uint8_t value)
{
  for (uint8_t bit = 0U; bit < 8U; ++bit)
  {
    oled_clk_low();

    if ((value & 0x80U) != 0U)
    {
      oled_mosi_high();
    }
    else
    {
      oled_mosi_low();
    }

    oled_bus_delay();
    oled_clk_high();
    oled_bus_delay();

    value <<= 1U;
  }

  oled_clk_low();
}

static void oled_write_command(uint8_t command)
{
  oled_dc_command();
  oled_cs1_select();
  oled_write_byte(command);
  oled_cs1_deselect();
}

static void oled_write_data(const uint8_t *data, size_t length)
{
  oled_dc_data();
  oled_cs1_select();

  for (size_t index = 0U; index < length; ++index)
  {
    oled_write_byte(data[index]);
  }

  oled_cs1_deselect();
}

void OLED_Init(void)
{
  oled_cs1_deselect();
  oled_cs2_deselect();
  oled_clk_low();
  oled_mosi_low();
  oled_dc_command();

  HAL_Delay(100U);

  oled_write_command(0xAEU);
  oled_write_command(0xD5U);
  oled_write_command(0x80U);
  oled_write_command(0xA8U);
  oled_write_command(0x3FU);
  oled_write_command(0xD3U);
  oled_write_command(0x00U);
  oled_write_command(0x40U);

#if OLED_CONTROLLER_SH1106
  oled_write_command(0xADU);
  oled_write_command(0x8BU);
#else
  oled_write_command(0x20U);
  oled_write_command(0x02U);
  oled_write_command(0x8DU);
  oled_write_command(0x14U);
#endif

  oled_write_command(0xA1U);
  oled_write_command(0xC8U);
  oled_write_command(0xDAU);
  oled_write_command(0x12U);
  oled_write_command(0x81U);
  oled_write_command(0x7FU);
  oled_write_command(0xD9U);
#if OLED_CONTROLLER_SH1106
  oled_write_command(0x22U);
#else
  oled_write_command(0xF1U);
#endif
  oled_write_command(0xDBU);
  oled_write_command(0x20U);
  oled_write_command(0xA4U);
  oled_write_command(0xA6U);
  oled_write_command(0xAFU);

  OLED_Clear();
  OLED_Update();
}

void OLED_Clear(void)
{
  (void)memset(oled_buffer, 0, sizeof(oled_buffer));
}

void OLED_Update(void)
{
  for (uint8_t page = 0U; page < OLED_PAGE_COUNT; ++page)
  {
    oled_write_command((uint8_t)(0xB0U + page));
    oled_write_command((uint8_t)(0x00U + (OLED_COLUMN_OFFSET & 0x0FU)));
    oled_write_command((uint8_t)(0x10U + ((OLED_COLUMN_OFFSET >> 4U) & 0x0FU)));
    oled_write_data(&oled_buffer[page * OLED_WIDTH], OLED_WIDTH);
  }
}

void OLED_SetPixel(uint8_t x, uint8_t y, uint8_t on)
{
  uint16_t index;
  uint8_t bit_mask;

  if ((x >= OLED_WIDTH) || (y >= OLED_HEIGHT))
  {
    return;
  }

  index = (uint16_t)x + ((uint16_t)(y / 8U) * OLED_WIDTH);
  bit_mask = (uint8_t)(1U << (y % 8U));

  if (on != 0U)
  {
    oled_buffer[index] |= bit_mask;
  }
  else
  {
    oled_buffer[index] &= (uint8_t)(~bit_mask);
  }
}

void OLED_DrawChar(uint8_t x, uint8_t y, char ch)
{
  const uint8_t *glyph;

  if (((uint16_t)x + 5U >= OLED_WIDTH) || (y >= OLED_HEIGHT))
  {
    return;
  }

  glyph = oled_get_glyph(ch);

  for (uint8_t column = 0U; column < 5U; ++column)
  {
    uint8_t bits = glyph[column];

    for (uint8_t row = 0U; row < 7U; ++row)
    {
      OLED_SetPixel((uint8_t)(x + column), (uint8_t)(y + row),
                    (uint8_t)((bits >> row) & 0x01U));
    }
  }

  for (uint8_t row = 0U; row < 7U; ++row)
  {
    OLED_SetPixel((uint8_t)(x + 5U), (uint8_t)(y + row), 0U);
  }
}

void OLED_DrawString(uint8_t x, uint8_t y, const char *text)
{
  uint8_t cursor_x = x;

  if (text == NULL)
  {
    return;
  }

  while (*text != '\0')
  {
    if ((uint16_t)cursor_x + 5U >= OLED_WIDTH)
    {
      break;
    }

    OLED_DrawChar(cursor_x, y, *text);
    cursor_x = (uint8_t)(cursor_x + 6U);
    ++text;
  }
}
