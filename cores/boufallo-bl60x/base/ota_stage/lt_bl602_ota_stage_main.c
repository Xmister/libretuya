#include <stdbool.h>
#include <stdint.h>
#include <bl_uart.h>
#include <utils_log.h>

extern bool lt_bl602_ota_stage_apply(void);

/* The standalone startup calls this instead of the SDK application entry.
 * It deliberately avoids starting the scheduler, Wi-Fi/BLE stacks, or app. */
#include "../api/lt_bl602_ota_stage.h"

void lt_bl602_ota_stage_main(void) {
	/* The standalone helper has its own startup and cannot assume UART0 remains
	 * configured after Boot2 jumps to it. Initialize the confirmed MINIR3 debug
	 * pins explicitly before the first marker; bl_uart_data_send() otherwise
	 * waits indefinitely for TX FIFO space if the peripheral is not initialized. */
	bl_uart_init(0, 16, 7, 0, 0, 2000000);
	bl_printk("[LT602STG] helper entry\r\n");
	const bool apply_ok = lt_bl602_ota_stage_apply();
	bl_printk("[LT602STG] apply returned=%u\r\n", (unsigned)apply_ok);
	if (!apply_ok) {
		bl_printk("[LT602STG] apply failed; trying fallback\r\n");
		if (!lt_bl602_ota_stage_revert()) {
			bl_printk("[LT602STG] fallback failed; halted\r\n");
			for (;;) {}
		}
	}
	for (;;) {}
}

/* The shared early startup object initializes the heap before constructors.
 * The standalone updater does not use FreeRTOS, so this is intentionally a
 * no-op. */
void lt_bl602_prepare_heap(void) {}

/* The pinned vendor archive's MFG-flash object references this optional
 * partition callback setter, but the updater writes the FW table through
 * hal_boot2_update_ptable(), which uses direct flash operations. */
void PtTable_Set_Flash_Operation(void *erase, void *write, void *read) {
	(void)erase;
	(void)write;
	(void)read;
}

int PtTable_Get_Active_Partition_Need_Lock(void *tables) {
	(void)tables;
	return 0;
}
