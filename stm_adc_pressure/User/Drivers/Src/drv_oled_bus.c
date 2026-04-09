#include "drv_oled_bus.h"

#include "main.h"
#include "spi.h"

#define DRV_OLED_SPI_TIMEOUT_MS 100U

static inline void drv_oled_dc_command(void)
{
  OLED_DC_GPIO_Port->BSRR = (uint32_t)OLED_DC_Pin << 16U;
}

static inline void drv_oled_dc_data(void)
{
  OLED_DC_GPIO_Port->BSRR = OLED_DC_Pin;
}

static inline void drv_oled_cs_low(void)
{
  OLED_CS_GPIO_Port->BSRR = (uint32_t)OLED_CS_Pin << 16U;
}

static inline void drv_oled_cs_high(void)
{
  OLED_CS_GPIO_Port->BSRR = OLED_CS_Pin;
}

static inline void drv_oled_rst_low(void)
{
  OLED_RST_GPIO_Port->BSRR = (uint32_t)OLED_RST_Pin << 16U;
}

static inline void drv_oled_rst_high(void)
{
  OLED_RST_GPIO_Port->BSRR = OLED_RST_Pin;
}

static void drv_oled_write_stream(const uint8_t *data, size_t length, uint8_t is_command)
{
  if ((data == NULL) || (length == 0U))
  {
    return;
  }

  if (is_command != 0U)
  {
    drv_oled_dc_command();
  }
  else
  {
    drv_oled_dc_data();
  }

  drv_oled_cs_low();
  (void)HAL_SPI_Transmit(&hspi3, (uint8_t *)data, (uint16_t)length, DRV_OLED_SPI_TIMEOUT_MS);

  drv_oled_cs_high();
  drv_oled_dc_data();
}

void DrvOledBus_Init(void)
{
  drv_oled_cs_high();
  drv_oled_dc_data();
  drv_oled_rst_low();
  HAL_Delay(100U);
  drv_oled_rst_high();
  HAL_Delay(10U);
}

void DrvOledBus_WriteCommand(uint8_t command)
{
  drv_oled_write_stream(&command, 1U, 1U);
}

void DrvOledBus_WriteCommandList(const uint8_t *commands, size_t length)
{
  drv_oled_write_stream(commands, length, 1U);
}

void DrvOledBus_WriteData(const uint8_t *data, size_t length)
{
  drv_oled_write_stream(data, length, 0U);
}
