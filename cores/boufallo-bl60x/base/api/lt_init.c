#include <FreeRTOS.h>
#include <bl_uart.h>
#include <libretiny.h>
#include <task.h>

extern uint8_t _heap_start, _heap_size, _heap_wifi_start, _heap_wifi_size;
/* SDK selects the hardware exchange-memory split from __LD_CONFIG_EM_SEL. */
extern int bl_sys_em_config(void);
static int heap_ready;
void __real_vPortDefineHeapRegions(const HeapRegion_t *regions);

void __wrap_vPortDefineHeapRegions(const HeapRegion_t *regions) {
	if (!heap_ready) {
		__real_vPortDefineHeapRegions(regions);
		heap_ready = 1;
	}
}

void lt_bl602_prepare_heap(void) {
	const HeapRegion_t regions[] = {
		{&_heap_start,		(size_t)&_heap_size		},
		{&_heap_wifi_start, (size_t)&_heap_wifi_size},
		{NULL,				0						},
	};
	__wrap_vPortDefineHeapRegions(regions);
}

/* No ROMFS exists on the stock MINIR3. Return failure to the SDK startup,
 * whose caller ignores it, rather than entering the media-partition trap. */
int __wrap_romfs_register(void) {
	return -1;
}

void lt_bl602_early_init(void) {
	/* The SDK's board main normally calls bl_sys_init(), which performs this
	 * selection. LibreTiny supplies its own main and must do it explicitly. */
	bl_sys_em_config();
	/* Reference board UART0 TX=GPIO16, RX=GPIO7. */
	bl_uart_init(0, 16, 7, 255, 255, 115200);
}
