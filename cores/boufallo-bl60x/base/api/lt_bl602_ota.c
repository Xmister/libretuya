#include <libretiny.h>
#include <lt_ota.h>
#include <sdk_private.h>

#include <bl_flash.h>
#include <hal_boot2.h>
#include <sodium/crypto_hash_sha256.h>

#define BL602_FW_HEADER_SIZE 0x1000
#define BL602_BOOT_HEADER_SIZE 176

extern void lt_bl602_ota_write_window(uint32_t offset, uint32_t length,
								  int mfg_overlap);
extern void lt_bl602_ota_clear_write_window(void);

static uint8_t ota_original_slot;
static uint8_t ota_target_slot;
static uint32_t ota_target_offset;
static uint32_t ota_target_capacity;
static bool ota_update_pending;

static uint32_t read_le32(const uint8_t *data) {
	return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
		((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static uint32_t crc32(const uint8_t *data, size_t length) {
	uint32_t crc = 0xFFFFFFFF;
	for (size_t i = 0; i < length; i++) {
		crc ^= data[i];
		for (uint8_t bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
	}
	return ~crc;
}

static bool get_fw_layout(uint32_t *address0, uint32_t *address1,
						  uint32_t *size0, uint32_t *size1, uint8_t *active) {
	int active_index = -1;
	if (hal_boot2_partition_addr("FW", address0, address1, size0, size1,
								&active_index) != 0 ||
		active_index < 0 || active_index > 1 || !*address0 || !*address1 ||
		!*size0 || !*size1)
		return false;
	*active = (uint8_t)active_index;
	return true;
}

static bool validate_slot(uint32_t offset, uint32_t capacity,
						  uint32_t *image_length) {
	uint8_t header[BL602_BOOT_HEADER_SIZE];
	if (capacity <= BL602_FW_HEADER_SIZE ||
		bl_flash_read(offset, header, (int)sizeof(header)) != 0 ||
		memcmp(header, "BFNP", 4) != 0 || crc32(header, 172) != read_le32(header + 172) ||
		(header[116] & 0x0F) != 0)
		return false;

	const uint32_t app_length = read_le32(header + 120);
	if (!app_length || app_length > capacity - BL602_FW_HEADER_SIZE)
		return false;

	crypto_hash_sha256_state sha;
	if (crypto_hash_sha256_init(&sha) != 0)
		return false;
	uint8_t data[1024];
	uint32_t remaining = app_length;
	uint32_t address = offset + BL602_FW_HEADER_SIZE;
	while (remaining) {
		const uint32_t chunk = remaining > sizeof(data) ? sizeof(data) : remaining;
		if (bl_flash_read(address, data, (int)chunk) != 0 ||
			crypto_hash_sha256_update(&sha, data, chunk) != 0)
			return false;
		address += chunk;
		remaining -= chunk;
	}
	uint8_t digest[crypto_hash_sha256_BYTES];
	if (crypto_hash_sha256_final(&sha, digest) != 0 ||
		memcmp(digest, header + 132, sizeof(digest)) != 0)
		return false;
	*image_length = BL602_FW_HEADER_SIZE + app_length;
	return true;
}

lt_ota_type_t lt_ota_get_type(void) {
	return OTA_TYPE_DUAL;
}

uint8_t lt_ota_dual_get_current(void) {
	uint32_t address0, address1, size0, size1;
	uint8_t active;
	return get_fw_layout(&address0, &address1, &size0, &size1, &active)
			? active + 1
			: 0;
}

uint8_t lt_ota_dual_get_stored(void) {
	return lt_ota_dual_get_current();
}

bool lt_ota_is_valid(uint8_t index) {
	uint32_t address0, address1, size0, size1, image_length;
	uint8_t active;
	if (index < 1 || index > 2 ||
		!get_fw_layout(&address0, &address1, &size0, &size1, &active))
		return false;
	(void)active;
	return validate_slot(index == 1 ? address0 : address1,
						 index == 1 ? size0 : size1, &image_length);
}

uf2_ota_scheme_t lt_ota_get_uf2_scheme(void) {
	const uint8_t current = lt_ota_dual_get_current();
	if (current == 1)
		return UF2_SCHEME_DEVICE_DUAL_2;
	if (current == 2)
		return UF2_SCHEME_DEVICE_DUAL_1;
	return UF2_SCHEME_DEVICE_DUAL_1;
}

void lt_ota_set_write_protect(uf2_ota_t *uf2) {
	uint32_t address0, address1, size0, size1;
	uint8_t active;
	ota_update_pending = false;
	if (!uf2 || !get_fw_layout(&address0, &address1, &size0, &size1, &active)) {
		lt_bl602_ota_clear_write_window();
		return;
	}
	ota_original_slot = active;
	ota_target_slot = 1 - active;
	ota_target_offset = ota_target_slot == 0 ? address0 : address1;
	ota_target_capacity = ota_target_slot == 0 ? size0 : size1;
	ota_update_pending = true;
	lt_bl602_ota_write_window(ota_target_offset, ota_target_capacity,
						  ota_target_slot == 1);
}

void lt_ota_clear_write_protect(void) {
	lt_bl602_ota_clear_write_window();
	ota_update_pending = false;
}

bool lt_ota_switch(bool revert) {
	if (revert) {
		lt_ota_clear_write_protect();
		return true;
	}
	if (!ota_update_pending)
		return false;

	uint32_t address0, address1, size0, size1, image_length;
	uint8_t active;
	if (!get_fw_layout(&address0, &address1, &size0, &size1, &active) ||
		active != ota_original_slot ||
		!validate_slot(ota_target_offset, ota_target_capacity, &image_length))
		return false;

	HALPartition_Entry_Config fw_entry = {0};
	if (hal_boot2_get_active_entries(BOOT2_PARTITION_TYPE_FW, &fw_entry) != 0 ||
		fw_entry.activeIndex != ota_original_slot)
		return false;
	fw_entry.activeIndex = ota_target_slot;
	fw_entry.len = image_length;
	if (hal_boot2_update_ptable(&fw_entry) != 0)
		return false;

	ota_update_pending = false;
	return true;
}
