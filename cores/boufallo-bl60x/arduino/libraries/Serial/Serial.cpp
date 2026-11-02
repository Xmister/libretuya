/* BL602 UART0; interrupt handling follows the MIT-licensed BL602 Arduino reference. */
#if LT_ARD_HAS_SERIAL
#include "SerialPrivate.h"
extern "C" {
#include <bl602_uart.h>
#include <bl_irq.h>
#include <bl_uart.h>
}

static void rx_notify(void *argument) {
	auto *data = static_cast<SerialData *>(argument);
	if (!data || !data->buffer)
		return;
	int value;
	while ((value = bl_uart_data_recv(0)) >= 0)
		data->buffer->store_char(static_cast<uint8_t>(value));
}

static SerialData *uart0_data;

static void uart0_irq(void) {
	if (UART_GetIntStatus(static_cast<UART_ID_Type>(0), UART_INT_RX_END) == SET ||
		UART_GetIntStatus(static_cast<UART_ID_Type>(0), UART_INT_RX_FIFO_REQ) == SET ||
		UART_GetIntStatus(static_cast<UART_ID_Type>(0), UART_INT_RTO) == SET) {
		rx_notify(uart0_data);
		UART_IntClear(static_cast<UART_ID_Type>(0), UART_INT_RX_END);
		UART_IntClear(static_cast<UART_ID_Type>(0), UART_INT_RTO);
		UART_IntClear(static_cast<UART_ID_Type>(0), UART_INT_RX_FER);
	}
}

void SerialClass::beginPrivate(unsigned long, uint16_t) {
	if (this->data)
		this->data->buffer = this->rxBuf;
}

void SerialClass::configure(unsigned long baudrate, uint16_t config) {
	if (!this->data || this->port != 0 || config != SERIAL_8N1)
		return;
	bl_uart_init(0, static_cast<uint8_t>(this->tx), static_cast<uint8_t>(this->rx), 255, 255, baudrate);
	uart0_data = this->data;
	bl_uart_int_rx_notify_register(0, rx_notify, this->data);
	bl_uart_int_rx_enable(0);
	bl_irq_register(UART0_IRQn, reinterpret_cast<void *>(uart0_irq));
	bl_irq_enable(UART0_IRQn);
	this->baudrate = baudrate;
	this->config   = config;
}

void SerialClass::endPrivate() {
	if (!this->data)
		return;
	bl_uart_int_rx_disable(0);
	bl_uart_int_rx_notify_unregister(0, rx_notify, this->data);
	uart0_data = nullptr;
}

void SerialClass::flush() {
	if (this->data)
		bl_uart_flush(0);
}

size_t SerialClass::write(uint8_t c) {
	if (!this->data)
		return 0;
	bl_uart_data_send(0, c);
	return 1;
}
#endif
