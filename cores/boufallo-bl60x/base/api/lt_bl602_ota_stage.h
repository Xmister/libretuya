#pragma once

#include <stdbool.h>
#include <stdint.h>

/* On-flash descriptor for the BL602 staged-image updater. All integer fields
 * are little-endian. Keep this wire format independent of compiler packing. */
#define LT_BL602_OTA_STAGE_MAGIC "LT602STG"
#define LT_BL602_OTA_STAGE_VERSION 1
#define LT_BL602_OTA_STAGE_MANIFEST_SIZE 96

typedef struct {
	uint32_t payload_offset;
	uint32_t payload_size;
	uint32_t image_size;
	uint8_t payload_sha256[32];
	uint8_t image_sha256[32];
} lt_bl602_ota_stage_manifest_t;

/* Read and structurally validate a stage descriptor in the currently booted
 * helper image. This only accepts payload bytes wholly inside the helper slot
 * and an expanded image that fits the explicitly supplied destination slot. */
bool lt_bl602_ota_stage_read_manifest(uint32_t stage_offset, uint32_t stage_capacity,
									  uint32_t helper_image_size,
									  uint32_t destination_capacity,
									  lt_bl602_ota_stage_manifest_t *manifest);

/* Apply an LZ4 block stored after the running BL602 image. The running image
 * must be the staged updater in the smaller OTA slot; the expanded image is
 * written to the other firmware slot. Returns false when no valid stage
 * manifest is present or when validation/application fails. */
bool lt_bl602_ota_stage_apply(void);

/* Re-select the original firmware slot when the staged image cannot be
 * applied and that slot still passes the BL602 image checks. */
bool lt_bl602_ota_stage_revert(void);
