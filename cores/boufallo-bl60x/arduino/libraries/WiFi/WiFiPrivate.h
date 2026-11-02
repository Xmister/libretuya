#pragma once
#include <ArduinoPrivate.h>
#include <WiFi.h>
extern "C" {
#include <FreeRTOS.h>
#include <aos/yloop.h>
#include <hal_wifi.h>
#include <lwip/netif.h>
#include <lwip/netifapi.h>
#include <lwip/tcpip.h>
#include <semphr.h>
#include <task.h>
#include <wifi_mgmr_ext.h>
}

/* Each SDK event is delivered on the yloop task. Configuration is copied
 * under the mutex before crossing to that task. */
struct BL602WiFiData {
	SemaphoreHandle_t lock;
	WiFiMode mode;
	WiFiStatus status;
	bool started, ready, ble_ready, pending_connect, pending_scan, auto_reconnect;
	bool use_dhcp, scan_passive, scan_hidden, scan_failed;
	bool pending_ap, ap_started, ap_hidden;
	uint8_t channel, scan_channel, bssid[6];
	uint8_t ap_channel;
	bool has_bssid;
	uint32_t ip, gw, mask, dns1, dns2, scan_ms, ap_ip, ap_gw, ap_mask;
	char ssid[33], password[65];
	char ap_ssid[33], ap_password[65];
};

#define BL_WIFI_DATA ((BL602WiFiData *)data)
