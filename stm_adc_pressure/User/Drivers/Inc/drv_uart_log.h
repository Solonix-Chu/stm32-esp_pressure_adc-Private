#ifndef DRV_UART_LOG_H
#define DRV_UART_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

void DrvUartLog_Init(void);
void DrvUartLog_Printf(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* DRV_UART_LOG_H */
