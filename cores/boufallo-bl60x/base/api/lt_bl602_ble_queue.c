/* BLE task to application task handoff; no callbacks or allocation per packet. */
#include "lt_bl602_ble_queue.h"
#include <errno.h>
#include <string.h>
#include <FreeRTOS.h>
#include <queue.h>

static QueueHandle_t events;
static QueueHandle_t ready_events;
static uint32_t dropped;
static uint32_t callback_reports;
static uint32_t queued_reports;
static uint32_t polled_reports;
static uint32_t generation;
static int scan_active;
int bl602_ble_queue_init(void) {
    if (events) return 0;
    events = xQueueCreate(BL602_BLE_QUEUE_LENGTH, sizeof(bl602_ble_event));
    if (!events) return -ENOMEM;
    ready_events = xQueueCreate(1, sizeof(bl602_ble_event));
    if (!ready_events) { vQueueDelete(events); events = NULL; return -ENOMEM; }
    return 0;
}
void bl602_ble_queue_begin_scan(void) {
    __atomic_store_n(&scan_active, 0, __ATOMIC_RELEASE);
    __atomic_add_fetch(&generation, 1, __ATOMIC_ACQ_REL);
    // Reclaim slots occupied by the previous session before accepting new
    // advertisements. The generation check also covers callbacks in flight.
    if (events) {
        bl602_ble_event stale;
        while (xQueueReceive(events, &stale, 0) == pdTRUE) {}
    }
    __atomic_store_n(&scan_active, 1, __ATOMIC_RELEASE);
}
void bl602_ble_queue_end_scan(void) {
    __atomic_store_n(&scan_active, 0, __ATOMIC_RELEASE);
    __atomic_add_fetch(&generation, 1, __ATOMIC_ACQ_REL);
}
void bl602_ble_queue_ready(int status) {
    if (!ready_events) return;
    bl602_ble_event event = {.type = BL602_BLE_EVENT_READY, .status = status};
    (void)xQueueOverwrite(ready_events, &event);
}
void bl602_ble_queue_advertisement(const uint8_t address[6], uint8_t address_type,
                                   int8_t rssi, uint8_t event_type,
                                   const uint8_t *data, size_t len) {
    uint32_t current_generation = __atomic_load_n(&generation, __ATOMIC_ACQUIRE);
    if (!__atomic_load_n(&scan_active, __ATOMIC_ACQUIRE)) return;
    if (!events || !address || (len && !data) || len > BL602_BLE_MAX_AD_BYTES) {
        __atomic_fetch_add(&dropped, 1, __ATOMIC_RELAXED);
        return;
    }
    __atomic_add_fetch(&callback_reports, 1, __ATOMIC_RELAXED);
    bl602_ble_event event = {.type = BL602_BLE_EVENT_ADVERTISEMENT, .address_type = address_type,
                             .rssi = rssi, .event_type = event_type, .data_len = (uint8_t)len,
                             .generation = current_generation};
    memcpy(event.address, address, sizeof(event.address));
    if (len) memcpy(event.data, data, len);
    if (xQueueSend(events, &event, 0) != pdTRUE)
        __atomic_fetch_add(&dropped, 1, __ATOMIC_RELAXED);
    else
        __atomic_fetch_add(&queued_reports, 1, __ATOMIC_RELAXED);
}
int bl602_ble_queue_poll(bl602_ble_event *event) {
    if (!events || !event) return -EINVAL;
    if (xQueueReceive(ready_events, event, 0) == pdTRUE) return 1;
    while (xQueueReceive(events, event, 0) == pdTRUE) {
        if (__atomic_load_n(&scan_active, __ATOMIC_ACQUIRE) &&
            event->generation == __atomic_load_n(&generation, __ATOMIC_ACQUIRE)) {
            __atomic_add_fetch(&polled_reports, 1, __ATOMIC_RELAXED);
            return 1;
        }
    }
    return 0;
}

uint32_t bl602_ble_queue_dropped(void) { return __atomic_load_n(&dropped, __ATOMIC_RELAXED); }
void bl602_ble_queue_get_stats(bl602_ble_queue_stats_t *stats) {
    if (!stats) return;
    stats->callback_reports = __atomic_load_n(&callback_reports, __ATOMIC_RELAXED);
    stats->queued_reports = __atomic_load_n(&queued_reports, __ATOMIC_RELAXED);
    stats->polled_reports = __atomic_load_n(&polled_reports, __ATOMIC_RELAXED);
    stats->dropped_reports = __atomic_load_n(&dropped, __ATOMIC_RELAXED);
}
