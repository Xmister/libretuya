#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Data is valid only during the callback; do not block the BLE task. */
typedef void (*bl602_scan_callback)(const uint8_t address[6], uint8_t address_type,
                                   int8_t rssi, uint8_t event_type,
                                   const uint8_t *data, size_t len);
typedef void (*bl602_scan_ready_callback)(int result);
typedef struct {
    uint32_t controller_reports;
} bl602_ble_scan_stats_t;
/* Initialize the BL602 BLE controller/host before Wi-Fi starts. The callback
 * runs from the SDK BLE task after the host stack is ready. */
int bl602_ble_prepare(bl602_scan_ready_callback ready);
/* Owns the BL602 BLE stack; must not be started alongside another BLE initializer. */
typedef struct {
    uint16_t interval; /* BLE units: 0.625 ms */
    uint16_t window;
    uint8_t active;
} bl602_scan_config;
/* Starts a scan after bl602_ble_prepare() reports stack readiness. */
int bl602_scan_start_configured(const bl602_scan_config *config,
                                bl602_scan_callback callback, bl602_scan_ready_callback ready);
int bl602_scan_start(bl602_scan_callback callback, bl602_scan_ready_callback ready);
int bl602_scan_stop(void);
/* Diagnostic snapshot; counters are reset when a scan is started. */
int bl602_ble_get_scan_stats(bl602_ble_scan_stats_t *stats);
/* Returns BLE identity in controller (least-significant-byte-first) order. */
int bl602_ble_get_address(uint8_t address[6]);
#ifdef __cplusplus
}
#endif
