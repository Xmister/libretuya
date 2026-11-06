/* Experimental interpretation of Sonoff BLE UUIDs.
 * No authentication/replay protection or packet deduplication is implied. */
#include "lt_bl602_sonoff.h"
static const uint32_t signatures[] = {
	0x8A6BECDA,
	0xD535B385,
	0x1FFC794F,
	0xE60781B6,
	0x2FCF487F,
	0xD734B087,
	0x02E36652,
	0xEB0B8FBB,
	0x32D15662,
	0xFA1B99AA,
	0x9575F6C5,
	0x9F7CFCCF,
	0x29C84B79,
	0xBF5FDDEF,
	0xA043C2F0,
	0xBF5EDAEF,
	0xA141C4F1,
	0x9D7EF8CD
};

static uint16_t fingerprint(uint32_t v) {
	return (uint16_t)((((v >> 24) ^ (v >> 16)) & 0xffu) << 8) | (uint16_t)(((v >> 8) ^ v) & 0xffu);
}

int sonoff_decode(uint32_t u2, uint32_t u3, uint32_t u4, sonoff_event *out) {
	if (!out)
		return 0;
	int match = -1;
	for (size_t i = 0; i < sizeof(signatures) / sizeof(signatures[0]); ++i) {
		if (fingerprint(u4) != fingerprint(signatures[i]))
			continue;
		if (match >= 0)
			return -1;
		match = (int)i;
	}
	if (match < 0)
		return 0;
	out->device = (u2 & 0xff000000u) | (u3 & 0x00ffffffu);
	out->button = (uint8_t)(match / 3 + 1);
	out->action = (uint8_t)(match % 3);
	return 1;
}

/* UUIDs are encoded little-endian within BLE AD structures. Preserve the
 * order across UUID lists, as used by the published Sonoff example. */
struct service_uuid {
	const uint8_t *bytes;
	size_t width;
};

static uint32_t le32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int sonoff_decode_advertisement(const uint8_t *data, size_t len, sonoff_event *out) {
	if (!data || !out)
		return -1;
	struct service_uuid uuids[16];
	size_t count = 0, offset = 0;
	while (offset < len) {
		size_t field_len = data[offset++];
		if (!field_len)
			break;
		if (field_len > len - offset)
			return -1;
		uint8_t type		   = data[offset];
		const uint8_t *payload = data + offset + 1;
		size_t payload_len = field_len - 1, width = 0;
		if (type == 0x02 || type == 0x03)
			width = 2;
		if (type == 0x04 || type == 0x05)
			width = 4;
		if (type == 0x06 || type == 0x07)
			width = 16;
		if (width) {
			if (payload_len % width)
				return -1;
			for (size_t i = 0; i < payload_len; i += width) {
				if (count == sizeof(uuids) / sizeof(uuids[0]))
					return -1;
				uuids[count++] = (struct service_uuid){payload + i, width};
			}
		}
		offset += field_len;
	}
	if (count < 6)
		return 0;
	int marker = 0;
	for (size_t i = 1; i < uuids[1].width; ++i)
		if (uuids[1].bytes[i - 1] == 0x78 && uuids[1].bytes[i] == 0xf6)
			marker = 1;
	if (!marker || uuids[2].width != 4 || uuids[3].width != 4 || uuids[4].width != 4)
		return 0;
	return sonoff_decode(le32(uuids[2].bytes), le32(uuids[3].bytes), le32(uuids[4].bytes), out);
}
