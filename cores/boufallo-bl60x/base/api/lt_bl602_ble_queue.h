#pragma once
#include "lt_bl602_ble.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define BL602_BLE_MAX_AD_BYTES 62
#define BL602_BLE_QUEUE_LENGTH 16

typedef enum { BL602_BLE_EVENT_READY, BL602_BLE_EVENT_ADVERTISEMENT } bl602_ble_event_type;
typedef struct {
    bl602_ble_event_type type;
    int status;
    uint32_t generation;
    uint8_t address[6];
    uint8_t address_type;
    int8_t rssi;
    uint8_t event_type;
    uint8_t data_len;
    uint8_t data[BL602_BLE_MAX_AD_BYTES];
} bl602_ble_event;
typedef struct {
    uint32_t callback_reports;
    uint32_t queued_reports;
    uint32_t polled_reports;
    uint32_t dropped_reports;
} bl602_ble_queue_stats_t;
/* Initialize once before starting the scanner. Returns 0 or a negative errno. */
int bl602_ble_queue_init(void);
/* Bound advertisement delivery to one scan session. Call from the main task
 * before starting a scan and after stopping it. */
void bl602_ble_queue_begin_scan(void);
void bl602_ble_queue_end_scan(void);
/* Callbacks for bl602_scan_start_configured; invoked in BLE stack task context. */
void bl602_ble_queue_ready(int status);
void bl602_ble_queue_advertisement(const uint8_t address[6], uint8_t address_type,
                                   int8_t rssi, uint8_t event_type,
                                   const uint8_t *data, size_t len);
/* Nonblocking; returns 1 if an event was received, 0 if empty, negative on error. */
int bl602_ble_queue_poll(bl602_ble_event *event);
/* Number of advertisements rejected or dropped since initialization. */
uint32_t bl602_ble_queue_dropped(void);
void bl602_ble_queue_get_stats(bl602_ble_queue_stats_t *stats);
#ifdef __cplusplus
}
#endif
