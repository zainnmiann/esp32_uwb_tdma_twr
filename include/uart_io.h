/**
 * @file uart_io.h
 * @brief Non-blocking UART console (ESP32 HardwareSerial).
 */
#ifndef UART_IO_H_
#define UART_IO_H_

#include <stdint.h>
#include <stdbool.h>

#define UART_TX_RING_SZ     4096u
#define UART_RX_LINE_SZ     160u

#ifdef __cplusplus
extern "C" {
#endif

void     uart_io_init(uint32_t baud);
bool     uart_io_baud_supported(uint32_t baud);
void     uart_io_write(const char *data, uint32_t len);
void     uart_io_puts(const char *s);
void     uart_io_printf(const char *fmt, ...);
void     uart_io_flush(void);
uint32_t uart_io_dropped(void);
char    *uart_io_poll_line(void);
uint32_t uart_io_rx_dropped(void);

#ifdef __cplusplus
}
#endif

#endif /* UART_IO_H_ */
