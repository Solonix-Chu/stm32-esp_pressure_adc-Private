#ifndef DRV_OLED_BUS_H
#define DRV_OLED_BUS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

void DrvOledBus_Init(void);
void DrvOledBus_WriteCommand(uint8_t command);
void DrvOledBus_WriteCommandList(const uint8_t *commands, size_t length);
void DrvOledBus_WriteData(const uint8_t *data, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* DRV_OLED_BUS_H */
