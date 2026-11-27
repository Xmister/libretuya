/* BL602 RAM size for the common LibreTiny memory API. */

#include <libretiny.h>

uint32_t lt_ram_get_size() {
	return 276 * 1024; // BL602: 276 KiB SRAM
}
