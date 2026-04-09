#ifndef COMP_OLED_H
#define COMP_OLED_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define COMP_OLED_WIDTH       128U
#define COMP_OLED_HEIGHT      64U
#define COMP_OLED_PAGE_COUNT  (COMP_OLED_HEIGHT / 8U)
#define COMP_OLED_BUFFER_SIZE (COMP_OLED_WIDTH * COMP_OLED_PAGE_COUNT)

void CompOled_Init(void);
void CompOled_Clear(void);
void CompOled_Update(void);
void CompOled_AllPixelsOn(uint8_t enable);
void CompOled_SetPixel(uint8_t x, uint8_t y, uint8_t on);
void CompOled_DrawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t on);
void CompOled_DrawChar(uint8_t x, uint8_t y, char ch);
void CompOled_DrawString(uint8_t x, uint8_t y, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* COMP_OLED_H */
