/* MINIR3 flash access. Mutations are restricted to the verified preferences
 * partition; the boot, firmware, OTA and recovery-copy regions stay read-only. */
#include <bl_flash.h>
#include <fal.h>
#include <libretiny.h>
#include <limits.h>
#define BL602_SECTOR_SIZE 4096

static int init(void) {
	return 0;
}

static int data_range(long offset, size_t len) {
	const unsigned long start = FLASH_DATA_OFFSET;
	const unsigned long end	  = start + FLASH_DATA_LENGTH;
	if (offset < 0 || (unsigned long)offset < start || (unsigned long)offset > end)
		return 0;
	return len <= end - (unsigned long)offset;
}

static int read_flash(long offset, uint8_t *data, size_t len) {
	if (offset < 0 || len > INT_MAX || (unsigned long)offset > FLASH_LENGTH ||
		len > FLASH_LENGTH - (unsigned long)offset)
		return -1;
	if (!len)
		return 0;
	return bl_flash_read((uint32_t)offset, data, (int)len) == 0 ? (int)len : -1;
}

static int write_flash(long offset, const uint8_t *data, size_t len) {
	if (offset < 0 || len > INT_MAX || !data_range(offset, len))
		return -1;
	if (!len)
		return 0;
	return bl_flash_write((uint32_t)offset, (uint8_t *)data, (int)len) == 0 ? (int)len : -1;
}

static int erase_flash(long offset, size_t len) {
	if (offset < 0 || len > INT_MAX || !data_range(offset, len))
		return -1;
	if (!len)
		return 0;
	if ((unsigned long)offset % BL602_SECTOR_SIZE || len % BL602_SECTOR_SIZE)
		return -1;
	return bl_flash_erase((uint32_t)offset, (int)len) == 0 ? (int)len : -1;
}
const struct fal_flash_dev flash0 = {
	.name		= FAL_FLASH_DEV_NAME,
	.addr		= 0,
	.len		= FLASH_LENGTH,
	.blk_size	= BL602_SECTOR_SIZE,
	.ops		= {init, read_flash, write_flash, erase_flash},
	.write_gran = 1,
};
