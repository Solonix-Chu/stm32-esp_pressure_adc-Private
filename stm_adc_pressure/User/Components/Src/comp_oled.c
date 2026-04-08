#include "comp_oled.h"

#include "drv_oled_bus.h"

#include <string.h>

static uint8_t comp_oled_buffer[COMP_OLED_BUFFER_SIZE];
static const uint8_t comp_oled_init_sequence[] = {
  0xAEU,
  0xD5U, 0x80U,
  0xA8U, 0x3FU,
  0xD3U, 0x00U,
  0x40U,
  0x8DU, 0x14U,
  0x20U, 0x02U,
  0xA1U,
  0xC0U,
  0xDAU, 0x12U,
  0x81U, 0xEFU,
  0xD9U, 0xF1U,
  0xDBU, 0x30U,
  0xA4U,
  0xA6U,
  0xAFU
};

static const uint8_t *comp_oled_get_glyph(char ch)
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

void CompOled_Init(void)
{
  DrvOledBus_Init();
  DrvOledBus_WriteCommandList(comp_oled_init_sequence, sizeof(comp_oled_init_sequence));
  CompOled_Clear();
  CompOled_Update();
}

void CompOled_Clear(void)
{
  (void)memset(comp_oled_buffer, 0, sizeof(comp_oled_buffer));
}

void CompOled_Update(void)
{
  for (uint8_t page = 0U; page < COMP_OLED_PAGE_COUNT; ++page)
  {
    DrvOledBus_WriteCommand((uint8_t)(0xB0U + page));
    DrvOledBus_WriteCommand(0x00U);
    DrvOledBus_WriteCommand(0x10U);
    DrvOledBus_WriteData(&comp_oled_buffer[page * COMP_OLED_WIDTH], COMP_OLED_WIDTH);
  }
}

void CompOled_AllPixelsOn(uint8_t enable)
{
  if (enable != 0U)
  {
    DrvOledBus_WriteCommand(0xA5U);
  }
  else
  {
    DrvOledBus_WriteCommand(0xA4U);
  }
}

void CompOled_SetPixel(uint8_t x, uint8_t y, uint8_t on)
{
  uint16_t index;
  uint8_t bit_mask;
  uint8_t page;

  if ((x >= COMP_OLED_WIDTH) || (y >= COMP_OLED_HEIGHT))
  {
    return;
  }

  page = (uint8_t)(7U - (y / 8U));
  index = (uint16_t)x + ((uint16_t)page * COMP_OLED_WIDTH);
  bit_mask = (uint8_t)(1U << (7U - (y % 8U)));

  if (on != 0U)
  {
    comp_oled_buffer[index] |= bit_mask;
  }
  else
  {
    comp_oled_buffer[index] &= (uint8_t)(~bit_mask);
  }
}

void CompOled_DrawChar(uint8_t x, uint8_t y, char ch)
{
  const uint8_t *glyph;

  if (((uint16_t)x + 5U >= COMP_OLED_WIDTH) || (y >= COMP_OLED_HEIGHT))
  {
    return;
  }

  glyph = comp_oled_get_glyph(ch);

  for (uint8_t column = 0U; column < 5U; ++column)
  {
    uint8_t bits = glyph[column];

    for (uint8_t row = 0U; row < 7U; ++row)
    {
      CompOled_SetPixel((uint8_t)(x + column), (uint8_t)(y + row),
                        (uint8_t)((bits >> row) & 0x01U));
    }
  }

  for (uint8_t row = 0U; row < 7U; ++row)
  {
    CompOled_SetPixel((uint8_t)(x + 5U), (uint8_t)(y + row), 0U);
  }
}

void CompOled_DrawString(uint8_t x, uint8_t y, const char *text)
{
  uint8_t cursor_x = x;

  if (text == NULL)
  {
    return;
  }

  while (*text != '\0')
  {
    if ((uint16_t)cursor_x + 5U >= COMP_OLED_WIDTH)
    {
      break;
    }

    CompOled_DrawChar(cursor_x, y, *text);
    cursor_x = (uint8_t)(cursor_x + 6U);
    ++text;
  }
}
