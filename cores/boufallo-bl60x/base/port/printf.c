#include <libretiny.h>
#include <bl_uart.h>
void putchar_p(char c, unsigned long port) { (void)bl_uart_data_send((uint8_t)port, (uint8_t)c); }
void putchar_(char c) { putchar_p(c, LT_UART_DEFAULT_LOGGER); }
