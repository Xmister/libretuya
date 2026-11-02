/* Experimental BL602 scanner. Callbacks execute in the SDK BLE task. */
#include "lt_bl602_ble.h"
#include "lt_bl602_wifi.h"
#include <FreeRTOS.h>
#include <ble_lib_api.h>
#include <bluetooth.h>
#include <errno.h>
#include <hci_driver.h>
#include <libretiny.h>
#include <string.h>

/* BLE-only builds may not include the Wi-Fi manager adapter. */
extern int lt_bl602_wifi_manager_ready_for_ble(void) __attribute__((weak));

static bl602_scan_callback on_advertisement;
static bl602_scan_ready_callback on_ready;

enum scan_state { OFF, INITIALIZING, IDLE, SCANNING, STOPPING, FAILED };

static int state;
static bl602_scan_config scan_config;
static uint32_t controller_reports;

static void scan_found(const bt_addr_le_t *address, int8_t rssi, uint8_t type, struct net_buf_simple *buf) {
	__atomic_add_fetch(&controller_reports, 1, __ATOMIC_RELAXED);
	if (on_advertisement && address && buf)
		on_advertisement(address->a.val, address->type, rssi, type, buf->data, buf->len);
}

static int begin_scan(void) {
	struct bt_le_scan_param params = {
		.type		= scan_config.active ? BT_LE_SCAN_TYPE_ACTIVE : BT_LE_SCAN_TYPE_PASSIVE,
		.filter_dup = 0,
		.interval	= scan_config.interval,
		.window		= scan_config.window,
	};
	LT_DM(BLE, "[BLEDBG] bt_le_scan_start enter");
	int error = bt_le_scan_start(&params, scan_found);
	LT_DM(BLE, "[BLEDBG] bt_le_scan_start exit=%d", error);
	__atomic_store_n(&state, error ? IDLE : SCANNING, __ATOMIC_RELEASE);
	return error;
}

static void stack_ready(int error) {
	LT_DM(BLE, "[BLEDBG] bt_enable callback enter=%d", error);
	if (error)
		__atomic_store_n(&state, FAILED, __ATOMIC_RELEASE);
	else
		__atomic_store_n(&state, IDLE, __ATOMIC_RELEASE);
	LT_DM(BLE, "[BLEDBG] bt_enable callback exit=%d", error);
	if (on_ready)
		on_ready(error);
}

int bl602_ble_prepare(bl602_scan_ready_callback ready) {
	if (!ready)
		return -EINVAL;
	int expected = OFF;
	if (!__atomic_compare_exchange_n(&state, &expected, INITIALIZING, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		return expected == IDLE || expected == SCANNING ? 0 : -EALREADY;
	on_ready = ready;
	LT_DM(BLE, "[BLEDBG] prepare controller priority=%u", (unsigned)(configMAX_PRIORITIES - 1));
	ble_controller_init(configMAX_PRIORITIES - 1);
	LT_DM(BLE, "[BLEDBG] controller initialized");
	hci_driver_init();
	LT_DM(BLE, "[BLEDBG] HCI initialized");
	int error = bt_enable(stack_ready);
	LT_DM(BLE, "[BLEDBG] bt_enable returned=%d", error);
	if (error)
		__atomic_store_n(&state, FAILED, __ATOMIC_RELEASE);
	return error;
}

int bl602_scan_start_configured(
	const bl602_scan_config *config,
	bl602_scan_callback callback,
	bl602_scan_ready_callback ready
) {
	if (!config || !callback || config->active > 1 || config->interval < 4 || config->interval > 0x4000 ||
		config->window < 4 || config->window > config->interval)
		return -EINVAL;
	int previous = __atomic_load_n(&state, __ATOMIC_ACQUIRE);
	for (;;) {
		if (previous == FAILED)
			return -EIO; /* Recovery requires a reboot. */
		if (previous == OFF || previous == INITIALIZING)
			return -EAGAIN;
		if (previous != IDLE)
			return -EALREADY;
		if (__atomic_compare_exchange_n(&state, &previous, INITIALIZING, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			break;
	}
	if (lt_bl602_wifi_manager_ready_for_ble && !lt_bl602_wifi_manager_ready_for_ble()) {
		__atomic_store_n(&state, previous, __ATOMIC_RELEASE);
		return -EAGAIN;
	}
	scan_config		 = *config;
	on_advertisement = callback;
	__atomic_store_n(&controller_reports, 0, __ATOMIC_RELAXED);
	(void)ready; /* Stack readiness is reported by bl602_ble_prepare(). */
	LT_DM(
		BLE,
		"[BLEDBG] scan config interval=%u window=%u active=%u",
		config->interval,
		config->window,
		config->active
	);
	LT_DM(BLE, "[BLEDBG] PTA left under SDK control");
	int error = begin_scan();
	return error;
}

int bl602_scan_start(bl602_scan_callback callback, bl602_scan_ready_callback ready) {
	const bl602_scan_config config = {240, 240, 0};
	return bl602_scan_start_configured(&config, callback, ready);
}

int bl602_scan_stop(void) {
	int previous = SCANNING;
	if (!__atomic_compare_exchange_n(&state, &previous, STOPPING, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		return previous == INITIALIZING || previous == STOPPING ? -EBUSY : -EINVAL;
	int error = bt_le_scan_stop();
	__atomic_store_n(&state, error ? SCANNING : IDLE, __ATOMIC_RELEASE);
	return error;
}

int bl602_ble_get_scan_stats(bl602_ble_scan_stats_t *stats) {
	if (!stats)
		return -EINVAL;
	stats->controller_reports = __atomic_load_n(&controller_reports, __ATOMIC_RELAXED);
	return 0;
}

int bl602_ble_get_address(uint8_t address[6]) {
	if (!address)
		return -EINVAL;
	int current = __atomic_load_n(&state, __ATOMIC_ACQUIRE);
	if (current != IDLE && current != SCANNING)
		return -EAGAIN;
	bt_addr_le_t identity;
	size_t count = 1;
	bt_id_get(&identity, &count);
	if (!count)
		return -ENOENT;
	memcpy(address, identity.a.val, 6);
	return 0;
}
