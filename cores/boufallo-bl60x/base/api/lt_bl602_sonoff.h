#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { uint32_t device; uint8_t button; uint8_t action; } sonoff_event;
/* uuid2, uuid3 and uuid4 are decoded 32-bit service UUID values; caller must
 * validate advertisement structure and the 0x78f6 marker before calling.
 * Returns 1 on a single match, 0 on no match, -1 on ambiguous match. */
int sonoff_decode(uint32_t uuid2, uint32_t uuid3, uint32_t uuid4, sonoff_event *out);
#ifdef __cplusplus
}
#endif
/* Parse a complete BLE AD buffer. Returns 1 for a decoded event, 0 for a
 * non-Sonoff/unmatched advertisement, -1 for malformed or ambiguous data. */
#ifdef __cplusplus
extern "C" {
#endif
int sonoff_decode_advertisement(const uint8_t *data, size_t len, sonoff_event *out);
#ifdef __cplusplus
}
#endif
