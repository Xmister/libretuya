#include "lt_bl602_ota_stage.h"

#include <bl_flash.h>
#include <bl_sec.h>
#include <bl_sys.h>
#include <hal_boot2.h>
/* The pinned SDK's mbedTLS SHA wrappers pass their context to bl_sha_*,
 * which uses bl_sha_ctx_t. Select the matching alternate context layout. */
#define MBEDTLS_SHA256_ALT
#include <mbedtls/sha256.h>
#include <stddef.h>
#include <string.h>
#include <utils_log.h>

#define LT_BL602_FW_HEADER_SIZE 0x1000U
#define LT_BL602_FW_XIP_ADDRESS 0x23000000U
#define LT_BL602_BOOT_HEADER_SIZE 176U
#define LT_BL602_FLASH_SECTOR_SIZE 4096U
#define LT_BL602_STAGE_IO_SIZE 256U
#define LT_BL602_STAGE_OUTPUT_SIZE 1024U
#define LT_BL602_LZ4_WINDOW_SIZE 65536U

/* Boot2 configures UART0 at 2 Mbps. The standalone helper deliberately uses
 * bl_printk directly because CFG_COMPONENT_BLOG_ENABLE is disabled in this
 * SDK build; blog_info() would compile to nothing. */
#define STAGE_LOG(...) bl_printk("[LT602STG] " __VA_ARGS__)

/* LZ4 block history; the updater runs from the other firmware slot while the
 * destination is erased and rewritten. */
static uint8_t lz4_window[LT_BL602_LZ4_WINDOW_SIZE];

typedef struct {
	const uint8_t *mapped;
	uint32_t remaining;
	uint16_t pos;
	uint16_t length;
	uint8_t buffer[LT_BL602_STAGE_IO_SIZE];
} stage_input_t;

typedef struct {
	uint32_t address;
	uint32_t capacity;
	uint32_t total;
	uint32_t next_progress;
	uint16_t used;
	bool write_flash;
	uint8_t buffer[LT_BL602_STAGE_OUTPUT_SIZE];
} stage_output_t;

/* The standalone helper uses a 16 KiB linker-allocated stack. Keep the large
 * decoder buffers and SHA state in BSS so nested SDK/hash/logging calls do not
 * depend on stack headroom. The SHA context is separate from stage_output so
 * resetting the output state cannot overwrite it. */
static stage_input_t stage_input;
static stage_output_t stage_output;
static mbedtls_sha256_context stage_sha;

static uint32_t read_le32(const uint8_t *data) {
	return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
		((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static uint16_t read_le16(const uint8_t *data) {
	return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t crc32(const uint8_t *data, size_t length) {
	uint32_t crc = 0xFFFFFFFFU;
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

bool lt_bl602_ota_stage_read_manifest(uint32_t stage_offset, uint32_t stage_capacity,
									  uint32_t helper_image_size,
									  uint32_t destination_capacity,
									  lt_bl602_ota_stage_manifest_t *manifest) {
	uint8_t raw[LT_BL602_OTA_STAGE_MANIFEST_SIZE];
	if (!manifest || helper_image_size > stage_capacity ||
		stage_capacity - helper_image_size < sizeof(raw) ||
		stage_offset > UINT32_MAX - stage_capacity ||
		bl_flash_read(stage_offset + helper_image_size, raw, sizeof(raw)) != 0)
		return false;

	if (memcmp(raw, LT_BL602_OTA_STAGE_MAGIC, 8) != 0 ||
		read_le16(raw + 8) != LT_BL602_OTA_STAGE_VERSION ||
		read_le16(raw + 10) != sizeof(raw) ||
		crc32(raw, sizeof(raw) - sizeof(uint32_t)) !=
			read_le32(raw + sizeof(raw) - sizeof(uint32_t)))
		return false;

	manifest->payload_offset = read_le32(raw + 12);
	manifest->payload_size = read_le32(raw + 16);
	manifest->image_size = read_le32(raw + 20);
	memcpy(manifest->payload_sha256, raw + 24, sizeof(manifest->payload_sha256));
	memcpy(manifest->image_sha256, raw + 56, sizeof(manifest->image_sha256));

	/* The expanded object is a complete BL602 firmware slot image, including
	 * its 0x1000-byte firmware header area. */
	if (!manifest->payload_size || !manifest->image_size ||
		manifest->payload_offset < helper_image_size + sizeof(raw) ||
		manifest->payload_offset > stage_capacity ||
		manifest->payload_size > stage_capacity - manifest->payload_offset ||
		manifest->image_size < LT_BL602_FW_HEADER_SIZE ||
		manifest->image_size > destination_capacity)
		return false;

	return true;
}

static bool input_byte(stage_input_t *input, uint8_t *value) {
	if (input->pos == input->length) {
		if (!input->remaining)
			return false;
		const uint16_t chunk = input->remaining > sizeof(input->buffer)
			? sizeof(input->buffer) : (uint16_t)input->remaining;
		memcpy(input->buffer, input->mapped, chunk);
		input->mapped += chunk;
		input->remaining -= chunk;
		input->pos = 0;
		input->length = chunk;
	}
	*value = input->buffer[input->pos++];
	return true;
}

static bool input_length(stage_input_t *input, uint32_t *length) {
	uint8_t next;
	while (input_byte(input, &next)) {
		if (*length > UINT32_MAX - next)
			return false;
		*length += next;
		if (next != 255)
			return true;
	}
	return false;
}

static bool output_flush(stage_output_t *output,
						 mbedtls_sha256_context *sha) {
	if (!output->used)
		return true;
	if (output->total == output->capacity) {
		STAGE_LOG("decode final flush used=%u crc=%08x\r\n",
			  (unsigned)output->used,
			  (unsigned)crc32(output->buffer, output->used));
	}
	for (uint16_t offset = 0; offset < output->used;
		 offset += LT_BL602_STAGE_IO_SIZE) {
		const uint16_t chunk = output->used - offset > LT_BL602_STAGE_IO_SIZE
			? LT_BL602_STAGE_IO_SIZE : output->used - offset;
		if (mbedtls_sha256_update_ret(sha, output->buffer + offset,
								  chunk) != 0)
			return false;
	}
	if ((output->total & 0xFFFFU) == 0)
		STAGE_LOG("sha checkpoint bytes=%u\r\n", (unsigned)output->total);
	if (output->write_flash &&
		bl_flash_write(output->address, output->buffer, output->used) != 0)
		return false;
	output->address += output->used;
	output->used = 0;
	return true;
}

static bool output_byte(stage_output_t *output,
						mbedtls_sha256_context *sha, uint8_t value) {
	if (output->total >= output->capacity)
		return false;
	lz4_window[output->total & (LT_BL602_LZ4_WINDOW_SIZE - 1)] = value;
	output->buffer[output->used++] = value;
	output->total++;
	if (output->total >= output->next_progress) {
		STAGE_LOG("decode write=%u progress=%u/%u\r\n",
			  (unsigned)output->write_flash, (unsigned)output->total,
			  (unsigned)output->capacity);
		output->next_progress += 0x1000U;
	}
	if ((output->total & (LT_BL602_LZ4_WINDOW_SIZE - 1)) == 0) {
		STAGE_LOG("decode checkpoint write=%u end=%u crc=%08x\r\n",
			  (unsigned)output->write_flash, (unsigned)output->total,
			  (unsigned)crc32(lz4_window, LT_BL602_LZ4_WINDOW_SIZE));
	}
	if (output->total == output->capacity &&
		(output->total & (LT_BL602_LZ4_WINDOW_SIZE - 1))) {
		const uint32_t tail = output->total & (LT_BL602_LZ4_WINDOW_SIZE - 1);
		STAGE_LOG("decode tail checkpoint write=%u end=%u crc=%08x\r\n",
			  (unsigned)output->write_flash, (unsigned)output->total,
			  (unsigned)crc32(lz4_window, tail));
	}
	return output->used != sizeof(output->buffer) || output_flush(output, sha);
}

/* Decode one raw LZ4 block and hash its expanded bytes. The first pass only
 * validates and hashes; the second pass writes after the full image is known
 * to be structurally valid and match its manifest digest. */
static bool decode_lz4(uint32_t payload_offset, uint32_t payload_size,
					   uint32_t image_size, uint32_t destination,
					   bool write_flash, uint8_t digest[32]) {
	memset(&stage_input, 0, sizeof(stage_input));
	stage_input.mapped = (const uint8_t *)(LT_BL602_FW_XIP_ADDRESS +
		payload_offset - LT_BL602_FW_HEADER_SIZE);
	stage_input.remaining = payload_size;
	memset(&stage_output, 0, sizeof(stage_output));
	stage_output.address = destination;
	stage_output.capacity = image_size;
	stage_output.next_progress = 0x1000U;
	stage_output.write_flash = write_flash;
	stage_input_t *input = &stage_input;
	stage_output_t *output = &stage_output;
	mbedtls_sha256_context *sha = &stage_sha;
	STAGE_LOG("decode start write=%u compressed=%u image=%u\r\n",
		  (unsigned)write_flash, (unsigned)payload_size, (unsigned)image_size);
	mbedtls_sha256_init(sha);
	if (mbedtls_sha256_starts_ret(sha, 0) != 0) {
		mbedtls_sha256_free(sha);
		return false;
	}

	bool ok = true;
	while (ok && input->remaining + input->length - input->pos) {
		uint8_t token;
		if (!input_byte(input, &token)) {
			ok = false;
			break;
		}
		uint32_t literal_length = token >> 4;
		if (literal_length == 15 && !input_length(input, &literal_length)) {
			ok = false;
			break;
		}
		if (literal_length > image_size - output->total) {
			ok = false;
			break;
		}
		for (uint32_t i = 0; ok && i < literal_length; i++) {
			uint8_t value;
			ok = input_byte(input, &value) && output_byte(output, sha, value);
		}
		if (!ok)
			break;

		/* A final literal-only sequence has no match offset. */
		if (!input->remaining && input->pos == input->length)
			break;
		uint8_t lo, hi;
		if (!input_byte(input, &lo) || !input_byte(input, &hi)) {
			ok = false;
			break;
		}
		const uint32_t offset = (uint32_t)lo | ((uint32_t)hi << 8);
		if (!offset || offset > output->total) {
			ok = false;
			break;
		}
		uint32_t match_length = (token & 0x0F) + 4;
		if ((token & 0x0F) == 15 && !input_length(input, &match_length)) {
			ok = false;
			break;
		}
		if (match_length > image_size - output->total) {
			ok = false;
			break;
		}
		for (uint32_t i = 0; ok && i < match_length; i++) {
			const uint8_t value = lz4_window[(output->total - offset) &
				(LT_BL602_LZ4_WINDOW_SIZE - 1)];
			ok = output_byte(output, sha, value);
		}
	}
	STAGE_LOG("decode loop end write=%u ok=%u output=%u input=%u+%u-%u used=%u\r\n",
		  (unsigned)write_flash, (unsigned)ok, (unsigned)output->total,
		  (unsigned)input->remaining, (unsigned)input->length,
		  (unsigned)input->pos, (unsigned)output->used);
	if (ok)
		ok = output->total == image_size && output_flush(output, sha);
	if (ok)
		ok = mbedtls_sha256_finish_ret(sha, digest) == 0;
	STAGE_LOG("decode return write=%u ok=%u output=%u\r\n",
	  (unsigned)write_flash, (unsigned)ok, (unsigned)output->total);
	mbedtls_sha256_free(sha);
	STAGE_LOG("decode cleanup done write=%u\r\n", (unsigned)write_flash);
	return ok;
}

static bool hash_mapped(const uint8_t *data, uint32_t length,
						uint8_t digest[32]) {
	mbedtls_sha256_context *sha = &stage_sha;
	mbedtls_sha256_init(sha);
	if (mbedtls_sha256_starts_ret(sha, 0) != 0) {
		mbedtls_sha256_free(sha);
		return false;
	}
	bool ok = true;
	while (length) {
		const uint32_t chunk = length > LT_BL602_STAGE_IO_SIZE ? LT_BL602_STAGE_IO_SIZE : length;
		if (mbedtls_sha256_update_ret(sha, data, chunk) != 0) {
			ok = false;
			break;
		}
		data += chunk;
		length -= chunk;
	}
	if (ok)
		ok = mbedtls_sha256_finish_ret(sha, digest) == 0;
	mbedtls_sha256_free(sha);
	return ok;
}

static bool read_running_image_size(uint32_t stage_offset, uint32_t capacity,
									uint32_t *image_size) {
	uint8_t header[LT_BL602_BOOT_HEADER_SIZE];
	if (capacity < LT_BL602_FW_HEADER_SIZE ||
		bl_flash_read(stage_offset, header, sizeof(header)) != 0 ||
		memcmp(header, "BFNP", 4) != 0 || crc32(header, 172) != read_le32(header + 172))
		return false;
	const uint32_t app_size = read_le32(header + 120);
	if (!app_size || app_size > capacity - LT_BL602_FW_HEADER_SIZE)
		return false;
	*image_size = LT_BL602_FW_HEADER_SIZE + app_size;
	return true;
}

static bool validate_slot(uint32_t offset, uint32_t capacity,
						  uint32_t *image_length) {
	uint8_t header[LT_BL602_BOOT_HEADER_SIZE];
	if (capacity <= LT_BL602_FW_HEADER_SIZE ||
		bl_flash_read(offset, header, sizeof(header)) != 0 ||
		memcmp(header, "BFNP", 4) != 0 || crc32(header, 172) != read_le32(header + 172) ||
		(header[116] & 0x0F) != 0)
		return false;

	const uint32_t app_length = read_le32(header + 120);
	if (!app_length || app_length > capacity - LT_BL602_FW_HEADER_SIZE)
		return false;
	mbedtls_sha256_context *sha = &stage_sha;
	mbedtls_sha256_init(sha);
	if (mbedtls_sha256_starts_ret(sha, 0) != 0) {
		mbedtls_sha256_free(sha);
		return false;
	}
	uint8_t data[LT_BL602_STAGE_IO_SIZE];
	uint32_t address = offset + LT_BL602_FW_HEADER_SIZE;
	uint32_t remaining = app_length;
	while (remaining) {
		const uint32_t chunk = remaining > sizeof(data) ? sizeof(data) : remaining;
		if (bl_flash_read(address, data, chunk) != 0 ||
			mbedtls_sha256_update_ret(sha, data, chunk) != 0) {
			mbedtls_sha256_free(sha);
			return false;
		}
		address += chunk;
		remaining -= chunk;
	}
	uint8_t digest[32];
	const int result = mbedtls_sha256_finish_ret(sha, digest);
	mbedtls_sha256_free(sha);
	if (result != 0 || memcmp(digest, header + 132, sizeof(digest)) != 0)
		return false;
	*image_length = LT_BL602_FW_HEADER_SIZE + app_length;
	return true;
}

bool lt_bl602_ota_stage_apply(void) {
	STAGE_LOG("apply begin\r\n");
	if (bl_flash_init() != 0) {
		STAGE_LOG("FAIL flash init\r\n");
		return false;
	}
	bl_sec_sha_init();
	uint32_t address0, address1, size0, size1, helper_size;
	uint8_t active;
	if (!get_fw_layout(&address0, &address1, &size0, &size1, &active)) {
		STAGE_LOG("FAIL read FW layout\r\n");
		return false;
	}
	const uint32_t stage_offset = active == 0 ? address0 : address1;
	const uint32_t stage_capacity = active == 0 ? size0 : size1;
	const uint32_t destination = active == 0 ? address1 : address0;
	const uint32_t destination_capacity = active == 0 ? size1 : size0;
	STAGE_LOG("layout active=%u slot0=%08x/%08x slot1=%08x/%08x\r\n",
			  active, (unsigned)address0, (unsigned)size0,
			  (unsigned)address1, (unsigned)size1);
	/* The helper belongs in the smaller OTA slot. The full firmware is
	 * expanded into the larger app slot; accepting the inverse placement can
	 * make a valid staged image impossible to apply. */
	if (stage_capacity >= destination_capacity) {
		STAGE_LOG("FAIL helper slot is not smaller (%08x >= %08x)\r\n",
				  (unsigned)stage_capacity, (unsigned)destination_capacity);
		return false;
	}
	if (!read_running_image_size(stage_offset, stage_capacity, &helper_size)) {
		STAGE_LOG("FAIL helper header at %08x\r\n", (unsigned)stage_offset);
		return false;
	}
	STAGE_LOG("helper valid at %08x size=%u\r\n",
			  (unsigned)stage_offset, (unsigned)helper_size);

	lt_bl602_ota_stage_manifest_t manifest;
	if (!lt_bl602_ota_stage_read_manifest(stage_offset, stage_capacity,
			helper_size, destination_capacity, &manifest)) {
		STAGE_LOG("FAIL manifest read/validation\r\n");
		return false;
	}
	STAGE_LOG("manifest payload=%08x+%u image=%u destination=%08x/%08x\r\n",
			  (unsigned)(stage_offset + manifest.payload_offset),
			  (unsigned)manifest.payload_size, (unsigned)manifest.image_size,
			  (unsigned)destination, (unsigned)destination_capacity);
	const uint32_t payload_address = stage_offset + manifest.payload_offset;
	uint8_t digest[32];
	const uint8_t *payload_xip = (const uint8_t *)(LT_BL602_FW_XIP_ADDRESS +
		manifest.payload_offset - LT_BL602_FW_HEADER_SIZE);
	if (!hash_mapped(payload_xip, manifest.payload_size, digest)) {
		STAGE_LOG("FAIL read/hash compressed payload\r\n");
		return false;
	}
	if (memcmp(digest, manifest.payload_sha256, sizeof(digest)) != 0) {
		STAGE_LOG("FAIL compressed payload SHA-256 expected=%02x%02x%02x%02x "
				  "actual=%02x%02x%02x%02x\r\n",
			  manifest.payload_sha256[0], manifest.payload_sha256[1],
			  manifest.payload_sha256[2], manifest.payload_sha256[3],
			  digest[0], digest[1], digest[2], digest[3]);
		return false;
	}
	STAGE_LOG("compressed payload SHA-256 OK\r\n");

	/* Validate the compressed stream and expanded hash before touching the
	 * destination. A reset during the later copy still returns to this helper. */
	const bool preflight_decoded = decode_lz4(manifest.payload_offset,
		manifest.payload_size, manifest.image_size, destination, false, digest);
	STAGE_LOG("preflight decode returned=%u\r\n", (unsigned)preflight_decoded);
	if (!preflight_decoded) {
		STAGE_LOG("FAIL preflight LZ4 decode\r\n");
		return false;
	}
	STAGE_LOG("preflight digest expected=%02x%02x%02x%02x actual=%02x%02x%02x%02x\r\n",
		  manifest.image_sha256[0], manifest.image_sha256[1],
		  manifest.image_sha256[2], manifest.image_sha256[3],
		  digest[0], digest[1], digest[2], digest[3]);
	if (memcmp(digest, manifest.image_sha256, sizeof(digest)) != 0) {
		STAGE_LOG("FAIL expanded image SHA-256\r\n");
		return false;
	}
	STAGE_LOG("preflight image hash OK\r\n");

	const uint32_t erase_length =
		(manifest.image_size + LT_BL602_FLASH_SECTOR_SIZE - 1) /
		LT_BL602_FLASH_SECTOR_SIZE * LT_BL602_FLASH_SECTOR_SIZE;
	if ((destination & (LT_BL602_FLASH_SECTOR_SIZE - 1)) ||
		erase_length > destination_capacity) {
		STAGE_LOG("FAIL erase destination=%08x length=%u\r\n",
				  (unsigned)destination, (unsigned)erase_length);
		return false;
	}
	STAGE_LOG("erase begin destination=%08x length=%u\r\n",
		  (unsigned)destination, (unsigned)erase_length);
	if (bl_flash_erase(destination, erase_length) != 0) {
		STAGE_LOG("FAIL erase destination=%08x length=%u\r\n",
			  (unsigned)destination, (unsigned)erase_length);
		return false;
	}
	STAGE_LOG("destination erased; writing %u bytes\r\n",
			  (unsigned)manifest.image_size);
	if (!decode_lz4(manifest.payload_offset, manifest.payload_size, manifest.image_size,
			destination, true, digest) ||
		memcmp(digest, manifest.image_sha256, sizeof(digest)) != 0) {
		STAGE_LOG("FAIL write/decode destination\r\n");
		return false;
	}
	STAGE_LOG("destination written and hashed\r\n");

	uint32_t verified_size;
	if (!validate_slot(destination, destination_capacity, &verified_size) ||
		verified_size != manifest.image_size) {
		STAGE_LOG("FAIL destination slot validation\r\n");
		return false;
	}
	STAGE_LOG("destination slot valid size=%u\r\n", (unsigned)verified_size);
	HALPartition_Entry_Config fw_entry = {0};
	if (hal_boot2_get_active_entries(BOOT2_PARTITION_TYPE_FW, &fw_entry) != 0 ||
		fw_entry.activeIndex != active) {
		STAGE_LOG("FAIL active FW table query/slot changed\r\n");
		return false;
	}
	/* hal_boot2_update_ptable() flips activeIndex itself. */
	fw_entry.len = manifest.image_size;
	STAGE_LOG("select expanded image from slot %u\r\n", (unsigned)(1U - active));
	if (hal_boot2_update_ptable(&fw_entry) != 0) {
		STAGE_LOG("FAIL partition table update\r\n");
		return false;
	}
	STAGE_LOG("partition table now selects slot %u; rebooting\r\n",
			  (unsigned)fw_entry.activeIndex);

	/* The staged updater is no longer needed; Boot2 will load the expanded app. */
	bl_sys_reset_por();
	for (;;) {}
}

bool lt_bl602_ota_stage_revert(void) {
	STAGE_LOG("fallback begin\r\n");
	if (bl_flash_init() != 0) {
		STAGE_LOG("FAIL fallback flash init\r\n");
		return false;
	}
	bl_sec_sha_init();
	uint32_t address0, address1, size0, size1, image_size;
	uint8_t active;
	if (!get_fw_layout(&address0, &address1, &size0, &size1, &active)) {
		STAGE_LOG("FAIL fallback read FW layout\r\n");
		return false;
	}
	const uint32_t fallback = active == 0 ? address1 : address0;
	const uint32_t fallback_capacity = active == 0 ? size1 : size0;
	STAGE_LOG("fallback current=%u target=%08x/%08x\r\n",
			  active, (unsigned)fallback, (unsigned)fallback_capacity);
	if (!validate_slot(fallback, fallback_capacity, &image_size)) {
		STAGE_LOG("FAIL fallback slot validation\r\n");
		return false;
	}
	HALPartition_Entry_Config fw_entry = {0};
	if (hal_boot2_get_active_entries(BOOT2_PARTITION_TYPE_FW, &fw_entry) != 0 ||
		fw_entry.activeIndex != active) {
		STAGE_LOG("FAIL fallback FW table query/slot changed\r\n");
		return false;
	}
	fw_entry.len = image_size;
	STAGE_LOG("reselect fallback slot %u size=%u\r\n",
			  (unsigned)(1U - active), (unsigned)image_size);
	if (hal_boot2_update_ptable(&fw_entry) != 0) {
		STAGE_LOG("FAIL fallback partition table update\r\n");
		return false;
	}
	STAGE_LOG("fallback table now selects slot %u; rebooting\r\n",
			  (unsigned)fw_entry.activeIndex);
	bl_sys_reset_por();
	for (;;) {}
}
