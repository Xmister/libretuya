#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Returns nonzero when BLE may initialize without racing Wi-Fi manager startup.
 * A missing Wi-Fi adapter means the application is BLE-only. */
int lt_bl602_wifi_manager_ready_for_ble(void);

#ifdef __cplusplus
}
#endif
