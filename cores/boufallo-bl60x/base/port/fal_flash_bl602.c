/* MINIR3 flash access. Normal mutations stay in preferences. During a validated
 * OTA, writes are additionally restricted to the inactive FW slot. */
#include <bl_flash.h>
#include <fal.h>
#include <hal_boot2.h>
#include <libretiny.h>
#include <limits.h>
#define BL602_SECTOR_SIZE 4096

static unsigned long ota_start;
static unsigned long ota_end;
static int ota_write_enabled;
static int ota_mfg_overlap;
static int ota_mfg_cleared;

void lt_bl602_ota_write_window(uint32_t offset, uint32_t length, int mfg_overlap) {
	ota_start = offset;
	ota_end = (unsigned long)offset + length;
	ota_write_enabled = length != 0 && ota_end <= FLASH_LENGTH;
	ota_mfg_overlap = mfg_overlap;
	ota_mfg_cleared = 0;
}

void lt_bl602_ota_clear_write_window(void) {
	ota_write_enabled = 0;
	ota_start = 0;
	ota_end = 0;
	ota_mfg_overlap = 0;
	ota_mfg_cleared = 0;
}

static int init(void) {
	return 0;
}

static int data_range(long offset, size_t len) {
	const unsigned long start = FLASH_DATA_OFFSET;
	const unsigned long end	  = start + FLASH_DATA_LENGTH;
	if (offset < 0)
		return 0;
	const unsigned long address = (unsigned long)offset;
	if (address >= start && address <= end && len <= end - address)
		return 1;
	return ota_write_enabled && address >= ota_start && address <= ota_end &&
		len <= ota_end - address;
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
	const unsigned long start =
		(unsigned long)offset / BL602_SECTOR_SIZE * BL602_SECTOR_SIZE;
	const unsigned long end =
		((unsigned long)offset + len + BL602_SECTOR_SIZE - 1) /
		BL602_SECTOR_SIZE * BL602_SECTOR_SIZE;
	if (end > FLASH_LENGTH || end - start > INT_MAX)
		return -1;
	if (ota_write_enabled && ota_mfg_overlap && !ota_mfg_cleared &&
		(unsigned long)offset >= ota_start &&
		(unsigned long)offset + len <= ota_end) {
		hal_update_mfg_ptable();
		ota_mfg_cleared = 1;
	}
	return bl_flash_erase((uint32_t)start, (int)(end - start)) == 0 ?
		(int)(end - start) : -1;
}
const struct fal_flash_dev flash0 = {
	.name		= FAL_FLASH_DEV_NAME,
	.addr		= 0,
	.len		= FLASH_LENGTH,
	.blk_size	= BL602_SECTOR_SIZE,
	.ops		= {init, read_flash, write_flash, erase_flash},
	.write_gran = 1,
};
