#include "drv_esp_link.h"

#include "main.h"
#include "spi.h"

void DrvEspLink_Init(void)
{
  DrvEspLink_Reset();
}

void DrvEspLink_SetReady(uint8_t ready)
{
  HAL_GPIO_WritePin(ESP_LINK_RDY_GPIO_Port,
                    ESP_LINK_RDY_Pin,
                    (ready != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

HAL_StatusTypeDef DrvEspLink_TransmitPacket(uint8_t *tx_buffer, uint8_t *rx_buffer, uint16_t length)
{
  HAL_StatusTypeDef status;

  if ((tx_buffer == NULL) || (rx_buffer == NULL) || (length == 0U))
  {
    return HAL_ERROR;
  }

  if (HAL_SPI_GetState(&hspi2) != HAL_SPI_STATE_READY)
  {
    (void)HAL_SPI_Abort(&hspi2);
  }

  __HAL_SPI_CLEAR_OVRFLAG(&hspi2);
  __HAL_SPI_CLEAR_FREFLAG(&hspi2);

  status = HAL_SPI_TransmitReceive_DMA(&hspi2, tx_buffer, rx_buffer, length);
  if (status == HAL_OK)
  {
    DrvEspLink_SetReady(1U);
  }
  else
  {
    DrvEspLink_SetReady(0U);
  }

  return status;
}

void DrvEspLink_Reset(void)
{
  DrvEspLink_SetReady(0U);
  (void)HAL_SPI_Abort(&hspi2);
  __HAL_SPI_CLEAR_OVRFLAG(&hspi2);
  __HAL_SPI_CLEAR_FREFLAG(&hspi2);
}
