#ifndef DRV_ESP_LINK_H
#define DRV_ESP_LINK_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f4xx_hal.h"

#include <stdint.h>

void DrvEspLink_Init(void);
void DrvEspLink_SetReady(uint8_t ready);
HAL_StatusTypeDef DrvEspLink_TransmitPacket(uint8_t *tx_buffer, uint8_t *rx_buffer, uint16_t length);
void DrvEspLink_Reset(void);

#ifdef __cplusplus
}
#endif

#endif /* DRV_ESP_LINK_H */
