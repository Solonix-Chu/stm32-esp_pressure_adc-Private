#include "drv_uart_log.h"

#include "usart.h"

#include <stdarg.h>
#include <stdio.h>

#define DRV_UART_LOG_BUFFER_SIZE 160U

static uint8_t drv_uart_log_initialized = 0U;

void DrvUartLog_Init(void)
{
  drv_uart_log_initialized = 1U;
}

void DrvUartLog_Printf(const char *fmt, ...)
{
  char buffer[DRV_UART_LOG_BUFFER_SIZE];
  int length;
  va_list args;

  if ((drv_uart_log_initialized == 0U) || (fmt == NULL))
  {
    return;
  }

  va_start(args, fmt);
  length = vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);

  if (length <= 0)
  {
    return;
  }

  if (length > (int)sizeof(buffer))
  {
    length = (int)sizeof(buffer);
  }

  (void)HAL_UART_Transmit(&huart1, (uint8_t *)buffer, (uint16_t)length, 100U);
}

int __io_putchar(int ch)
{
  uint8_t byte = (uint8_t)ch;

  if (drv_uart_log_initialized != 0U)
  {
    (void)HAL_UART_Transmit(&huart1, &byte, 1U, 100U);
  }

  return ch;
}
