/* Experimental BL602 Wi-Fi adapter. The event path and radio coexistence
 * require testing on hardware; this is not a deployable board definition. */
#ifdef BL602_WIFI_HOST_TEST
#include <WiFiMock.h>
#else
#include "WiFiPrivate.h"
#endif
#include <string.h>

static wifi_conf_t wifi_conf = { .country_code = "CN" };
static void wifi_event(input_event_t *event, void *arg);
static void scan_done(void *arg, void *unused);
static void start_pending(WiFiClass *wifi);

// SDK global constructors run before bfl_main defines FreeRTOS heap regions.
// The singleton must not allocate until the application task enables Wi-Fi.
WiFiClass::WiFiClass() : data(NULL) {}
void WiFiClass::dataInitialize() {
    if (data) return;
    auto *d = static_cast<BL602WiFiData *>(calloc(1, sizeof(BL602WiFiData)));
    if (!d) return;
    d->lock = xSemaphoreCreateMutex();
    if (!d->lock) { free(d); return; }
    d->status = WL_DISCONNECTED;
    d->use_dhcp = true;
    data = d;
}
WiFiClass::~WiFiClass() {
    if (data) {
        if (BL_WIFI_DATA->lock) vSemaphoreDelete(BL_WIFI_DATA->lock);
        free(data);
        data = NULL;
    }
}

static void emit(EventId event, EventInfo &info) { WiFi.postEvent(event, info); }
static void emit_simple(EventId event) { EventInfo info = {}; emit(event, info); }
static bool lock(BL602WiFiData *d) { return d && d->lock && xSemaphoreTake(d->lock, portMAX_DELAY) == pdTRUE; }
static void unlock(BL602WiFiData *d) { xSemaphoreGive(d->lock); }

static void wifi_event(input_event_t *event, void *arg) {
    auto *wifi = static_cast<WiFiClass *>(arg);
    auto *d = static_cast<BL602WiFiData *>(wifi->data);
    if (!event || !lock(d)) return;
    switch (event->code) {
    case CODE_WIFI_ON_INIT_DONE:
        unlock(d); wifi_mgmr_start_background(&wifi_conf); return;
    case CODE_WIFI_ON_MGMR_DONE: {
        d->ready = true;
        bool reconnect = d->auto_reconnect;
        unlock(d);
        if (reconnect) wifi_mgmr_sta_autoconnect_enable();
        else wifi_mgmr_sta_autoconnect_disable();
        start_pending(wifi);
        if (lock(d)) { d->ble_ready = true; unlock(d); }
        return;
    }
    case CODE_WIFI_ON_SCAN_DONE:
        if (event->value == WIFI_SCAN_DONE_EVENT_BUSY && wifi->scan &&
            wifi->scan->running && !d->pending_scan) {
            d->scan_failed = true;
            wifi->scan->running = false;
            EventInfo info = {};
            info.wifi_scan_done.status = 1;
            unlock(d); emit(ARDUINO_EVENT_WIFI_SCAN_DONE, info); return;
        }
        unlock(d); return;
    case CODE_WIFI_ON_CONNECTING:
        d->status = WL_IDLE_STATUS; unlock(d); return;
    case CODE_WIFI_ON_CONNECTED: {
        EventInfo info = {};
        wifi_mgmr_sta_connect_ind_stat_info_t actual = {};
        wifi_mgmr_sta_connect_ind_stat_get(&actual);
        size_t n = strnlen(actual.ssid, sizeof(actual.ssid));
        memcpy(info.wifi_sta_connected.ssid, actual.ssid, n);
        info.wifi_sta_connected.ssid_len = n;
        memcpy(d->bssid, actual.bssid, sizeof(d->bssid));
        memcpy(info.wifi_sta_connected.bssid, actual.bssid, sizeof(d->bssid));
        info.wifi_sta_connected.channel = actual.chan_id;
        unlock(d); emit(ARDUINO_EVENT_WIFI_STA_CONNECTED, info); return;
    }
    case CODE_WIFI_ON_GOT_IP:
        d->status = WL_CONNECTED; unlock(d); emit_simple(ARDUINO_EVENT_WIFI_STA_GOT_IP); return;
    case CODE_WIFI_ON_AP_STARTED:
        d->ap_started = true; unlock(d); emit_simple(ARDUINO_EVENT_WIFI_AP_START); return;
    case CODE_WIFI_ON_DISCONNECT: {
        d->status = WL_DISCONNECTED;
        EventInfo info = {};
        info.wifi_sta_disconnected.reason = WIFI_REASON_UNSPECIFIED;
        unlock(d); emit(ARDUINO_EVENT_WIFI_STA_DISCONNECTED, info); return;
    }
    default: unlock(d); return;
    }
}

static void start_pending(WiFiClass *wifi) {
    auto *d = static_cast<BL602WiFiData *>(wifi->data);
    if (!lock(d)) return;
    bool connect = d->ready && d->pending_connect;
    bool scan = d->ready && d->pending_scan;
    bool ap = d->ready && d->pending_ap;
    if (connect) d->pending_connect = false;
    if (scan) d->pending_scan = false;
    if (ap) d->pending_ap = false;
    /* SDK copies connect fields before returning; serialize them against begin. */
    bool connect_failed = false;
    if (connect) {
        wifi_interface_t iface = wifi_mgmr_sta_enable();
#if LWIP_NETIF_HOSTNAME
        netif_set_hostname(wifi_mgmr_sta_netif_get(), wifi->getHostname());
#endif
        if (!d->use_dhcp) wifi_mgmr_sta_ip_set(d->ip, d->mask, d->gw, d->dns1, d->dns2);
        int rc = wifi_mgmr_sta_connect_mid(&iface, d->ssid, d->password[0] ? d->password : NULL,
                                           NULL, d->has_bssid ? d->bssid : NULL, 0, d->channel,
                                           d->use_dhcp, WIFI_CONNECT_PMF_CAPABLE);
        if (rc) { d->status = WL_CONNECT_FAILED; connect_failed = true; }
    }
    if (scan) {
        // Scanning is often the first operation after startup; create the STA
        // interface even when no connection has been requested yet.
        wifi_mgmr_sta_enable();
#if LWIP_NETIF_HOSTNAME
        netif_set_hostname(wifi_mgmr_sta_netif_get(), wifi->getHostname());
#endif
        wifi_mgmr_scan_filter_hidden_ssid(d->scan_hidden ? 0 : 1);
        static const uint8_t broadcast[6] = {255,255,255,255,255,255};
        uint16_t channel = d->scan_channel;
        if (wifi_mgmr_scan_adv(wifi, scan_done, channel ? &channel : NULL, channel ? 1 : 0,
                               broadcast, NULL, d->scan_passive ? 0 : 1, d->scan_ms * 1000) != 0) {
            d->scan_failed = true;
            if (wifi->scan) wifi->scan->running = false;
            unlock(d);
            EventInfo info = {}; info.wifi_scan_done.status = 1;
            emit(ARDUINO_EVENT_WIFI_SCAN_DONE, info);
            return;
        }
    }
    if (ap) {
        wifi_interface_t iface = wifi_mgmr_ap_enable();
        struct netif *ap_netif = wifi_mgmr_ap_netif_get();
        if (ap_netif) {
            ip4_addr_t ip, gw, mask;
            ip.addr = d->ap_ip;
            gw.addr = d->ap_gw;
            mask.addr = d->ap_mask;
            netifapi_netif_set_addr(ap_netif, &ip, &mask, &gw);
        }
        if (wifi_mgmr_ap_start_adv(&iface, d->ap_ssid, d->ap_hidden,
                                   d->ap_password[0] ? d->ap_password : NULL,
                                   d->ap_channel, 1) != 0)
            d->ap_started = false;
    }
    unlock(d);
    if (connect_failed) {
        EventInfo info = {};
        info.wifi_sta_disconnected.reason = WIFI_REASON_CONNECTION_FAIL;
        emit(ARDUINO_EVENT_WIFI_STA_DISCONNECTED, info);
    }
}

bool WiFiClass::modePriv(WiFiMode mode, WiFiModeAction sta_action, WiFiModeAction ap_action) {
    if (mode != WIFI_MODE_NULL && mode != WIFI_MODE_STA && mode != WIFI_MODE_AP && mode != WIFI_MODE_APSTA) return false;
    if (!data && mode == WIFI_MODE_NULL) return true;
    if (!data) dataInitialize();
    auto *d = BL_WIFI_DATA;
    if (!lock(d)) return false;
    bool start = mode != WIFI_MODE_NULL && !d->started;
    bool ready = d->ready;
    d->mode = mode;
    if (start) d->started = true;
    if (sta_action == WLMODE_DISABLE) {
        d->pending_connect = false;
        d->status = WL_DISCONNECTED;
    }
    if (ap_action == WLMODE_DISABLE) {
        d->pending_ap = false;
        d->ap_started = false;
    }
    bool canceled_scan = mode == WIFI_MODE_NULL && d->pending_scan && scan && scan->running;
    if (mode == WIFI_MODE_NULL) {
        d->pending_connect = false;
        d->pending_scan = false;
        d->status = WL_DISCONNECTED;
        if (canceled_scan) { d->scan_failed = true; scan->running = false; }
    }
    unlock(d);
    if (canceled_scan) {
        EventInfo info = {};
        info.wifi_scan_done.status = 1;
        emit(ARDUINO_EVENT_WIFI_SCAN_DONE, info);
    }
    if (start) {
        tcpip_init(NULL, NULL);
        wifi_mgmr_cli_init();
        if (aos_register_event_filter(EV_WIFI, wifi_event, this) != 0) {
            if (lock(d)) { d->started = false; d->mode = WIFI_MODE_NULL; unlock(d); }
            return false;
        }
        hal_wifi_start_firmware_task();
        aos_post_event(EV_WIFI, CODE_WIFI_ON_INIT_DONE, 0);
        emit_simple(ARDUINO_EVENT_WIFI_STA_START);
    } else if (ready) {
        if (sta_action == WLMODE_DISABLE || mode == WIFI_MODE_NULL) {
            wifi_mgmr_sta_disconnect();
        }
        if (ap_action == WLMODE_DISABLE || mode == WIFI_MODE_NULL) {
            wifi_mgmr_ap_stop(NULL);
        }
    }
    return true;
}
WiFiMode WiFiClass::getMode() { auto *d=BL_WIFI_DATA; if(!lock(d)) return WIFI_MODE_NULL; auto m=d->mode; unlock(d); return m; }
WiFiStatus WiFiClass::status() { auto *d=BL_WIFI_DATA; if(!lock(d)) return WL_DISCONNECTED; auto s=d->status; unlock(d); return s; }
WiFiStatus WiFiClass::begin(const char *ssid,const char *pass,int32_t channel,const uint8_t *bssid,bool connect) {
    if ((pass && strlen(pass) > 63) || !validate(ssid, pass) || channel < 0 || channel > 14 || !enableSTA(true)) return WL_CONNECT_FAILED;
    auto *d=BL_WIFI_DATA; if (!lock(d)) return WL_CONNECT_FAILED;
    strncpy(d->ssid,ssid,sizeof(d->ssid)-1); d->ssid[32]=0;
    if(pass) { strncpy(d->password,pass,sizeof(d->password)-1); d->password[64]=0; } else d->password[0]=0;
    d->channel=channel; d->has_bssid=bssid!=NULL; if(bssid) memcpy(d->bssid,bssid,6);
    d->pending_connect=connect; d->status=connect ? WL_IDLE_STATUS : WL_DISCONNECTED;
    unlock(d); if(connect) start_pending(this);
    return status();
}
bool WiFiClass::disconnect(bool off) {
    auto *d=BL_WIFI_DATA; if(!lock(d)) return false;
    d->pending_connect=false; d->status=WL_DISCONNECTED; bool ready=d->ready; unlock(d);
    bool ok=!ready || wifi_mgmr_sta_disconnect()==0;
    if(off) ok=enableSTA(false) && ok;
    return ok;
}
bool WiFiClass::setAutoReconnect(bool value) {
    if (!data) dataInitialize();
    auto *d = BL_WIFI_DATA;
    if (!lock(d)) return false;
    d->auto_reconnect = value;
    bool ready = d->ready;
    unlock(d);
    return !ready || (value ? wifi_mgmr_sta_autoconnect_enable() : wifi_mgmr_sta_autoconnect_disable()) == 0;
}
extern "C" int lt_bl602_wifi_manager_ready_for_ble(void) {
    auto *d = static_cast<BL602WiFiData *>(WiFi.data);
    if (!d) return 1;  // BLE-only configurations never start the Wi-Fi manager.
    if (!lock(d)) return 0;
    // Do not initialize the BLE controller while Wi-Fi is still associating.
    // On BL602 both radios share the RF path, and starting BLE during the
    // hidden-network scan can stall Wi-Fi before it obtains an address. The
    // scanner retries this gate from its normal loop after GOT_IP.
    bool ready = !d->started || (d->ble_ready && d->status == WL_CONNECTED);
    unlock(d);
    return ready;
}
bool WiFiClass::config(IPAddress ip,IPAddress gateway,IPAddress mask,IPAddress dns1,IPAddress dns2) {
    if (!data) dataInitialize();
    auto *d=BL_WIFI_DATA; if(!lock(d)) return false;
    d->ip=ip; d->gw=gateway; d->mask=mask; d->dns1=dns1; d->dns2=dns2; d->use_dhcp=(uint32_t)ip==0;
    bool ready=d->ready; bool dhcp=d->use_dhcp; unlock(d);
    if(!ready) return true;
    return (dhcp ? wifi_mgmr_sta_ip_unset() : wifi_mgmr_sta_ip_set(ip,mask,gateway,dns1,dns2))==0;
}
const char *WiFiClass::getHostname() { return staHostname; }
bool WiFiClass::setHostname(const char *name) {
    if (name != staHostname && !storeHostname(staHostname,name)) return false;
#if LWIP_NETIF_HOSTNAME
    auto *d=BL_WIFI_DATA;
    if (!d) return true;
    if(!lock(d)) return false;
    bool ready=d->ready;
    unlock(d);
    if (ready) netif_set_hostname(wifi_mgmr_sta_netif_get(), staHostname);
    return true;
#else
    return false;
#endif
}
IPAddress WiFiClass::localIP() { uint32_t ip=0,gw=0,mask=0; wifi_mgmr_sta_ip_get(&ip,&gw,&mask); return IPAddress(ip); }
IPAddress WiFiClass::gatewayIP() { uint32_t ip=0,gw=0,mask=0; wifi_mgmr_sta_ip_get(&ip,&gw,&mask); return IPAddress(gw); }
IPAddress WiFiClass::subnetMask() { uint32_t ip=0,gw=0,mask=0; wifi_mgmr_sta_ip_get(&ip,&gw,&mask); return IPAddress(mask); }
IPAddress WiFiClass::dnsIP(uint8_t n) { uint32_t a=0,b=0; if(n>1 || wifi_mgmr_sta_dns_get(&a,&b)) return IPAddress(); return IPAddress(n?b:a); }
uint8_t *WiFiClass::macAddress(uint8_t *mac) { return mac && wifi_mgmr_sta_mac_get(mac)==0 ? mac : NULL; }
const String WiFiClass::SSID() { auto *d=BL_WIFI_DATA; if(!lock(d)) return String(); String s(d->ssid); unlock(d); return s; }
uint8_t *WiFiClass::BSSID() { return BL_WIFI_DATA ? BL_WIFI_DATA->bssid : NULL; }
int32_t WiFiClass::channel() { int n=0; return wifi_mgmr_channel_get(&n)==0 ? n : 0; }
int8_t WiFiClass::RSSI() { int n=0; return wifi_mgmr_rssi_get(&n)==0 ? n : 0; }

bool WiFiClass::softAPConfig(IPAddress localIP, IPAddress gateway, IPAddress subnet) {
    if (!data) dataInitialize();
    auto *d = BL_WIFI_DATA;
    if (!lock(d)) return false;
    d->ap_ip = localIP;
    d->ap_gw = gateway;
    d->ap_mask = subnet;
    unlock(d);
    return true;
}

bool WiFiClass::softAP(const char *ssid, const char *passphrase, int channel, bool hidden, int maxClients) {
    (void) maxClients;  // The SDK AP API uses its configured default client limit.
    if (!ssid || !*ssid || strlen(ssid) > 32 || channel < 1 || channel > 14 ||
        (passphrase && (strlen(passphrase) < 8 || strlen(passphrase) > 63)))
        return false;
    if (!data) dataInitialize();
    auto *d = BL_WIFI_DATA;
    if (!lock(d)) return false;
    strncpy(d->ap_ssid, ssid, sizeof(d->ap_ssid) - 1);
    d->ap_ssid[sizeof(d->ap_ssid) - 1] = 0;
    if (passphrase) {
        strncpy(d->ap_password, passphrase, sizeof(d->ap_password) - 1);
        d->ap_password[sizeof(d->ap_password) - 1] = 0;
    } else {
        d->ap_password[0] = 0;
    }
    d->ap_channel = channel;
    d->ap_hidden = hidden;
    d->pending_ap = true;
    bool ready = d->ready;
    unlock(d);
    return !ready || (start_pending(this), true);
}

bool WiFiClass::softAPdisconnect(bool wifiOff) {
    auto *d = BL_WIFI_DATA;
    if (!lock(d)) return false;
    d->pending_ap = false;
    bool ready = d->ready;
    d->ap_started = false;
    unlock(d);
    bool ok = !ready || wifi_mgmr_ap_stop(NULL) == 0;
    if (wifiOff) ok = mode(WIFI_MODE_NULL) && ok;
    return ok;
}

uint8_t WiFiClass::softAPgetStationNum() {
    uint8_t count = 0;
    return wifi_mgmr_ap_sta_cnt_get(&count) == 0 ? count : 0;
}

IPAddress WiFiClass::softAPIP() {
    uint32_t ip = 0, gw = 0, mask = 0;
    wifi_mgmr_ap_ip_get(&ip, &gw, &mask);
    return IPAddress(ip);
}

IPAddress WiFiClass::softAPSubnetMask() {
    uint32_t ip = 0, gw = 0, mask = 0;
    wifi_mgmr_ap_ip_get(&ip, &gw, &mask);
    return IPAddress(mask);
}

static void scan_item(wifi_mgmr_ap_item_t *, uint32_t *arg, wifi_mgmr_ap_item_t *item) {
    auto *wifi = reinterpret_cast<WiFiClass *>(arg);
    auto *d = static_cast<BL602WiFiData *>(wifi->data);
    if (!item || !wifi->scan || wifi->scan->count == 255 || d->scan_failed) return;
    unsigned index = wifi->scan->count;
    if (wifi->scanAlloc(index + 1) != index + 1) { d->scan_failed = true; return; }
    WiFiScanAP &ap = wifi->scan->ap[index];
    unsigned n = item->ssid_len > 32 ? 32 : item->ssid_len;
    ap.ssid = static_cast<char *>(malloc(n + 1));
    if (!ap.ssid) { d->scan_failed = true; return; }
    memcpy(ap.ssid, item->ssid, n); ap.ssid[n] = 0;
    memcpy(ap.bssid.addr, item->bssid, 6);
    ap.channel = item->channel; ap.rssi = item->rssi;
    switch (item->auth) {
      case WM_WIFI_AUTH_OPEN: ap.auth = WIFI_AUTH_OPEN; break;
      case WM_WIFI_AUTH_WEP: ap.auth = WIFI_AUTH_WEP; break;
      case WM_WIFI_AUTH_WPA_PSK: ap.auth = WIFI_AUTH_WPA_PSK; break;
      case WM_WIFI_AUTH_WPA2_PSK: ap.auth = WIFI_AUTH_WPA2_PSK; break;
      case WM_WIFI_AUTH_WPA_WPA2_PSK: ap.auth = WIFI_AUTH_WPA_WPA2_PSK; break;
      case WM_WIFI_AUTH_WPA3_SAE: ap.auth = WIFI_AUTH_WPA3_PSK; break;
      case WM_WIFI_AUTH_WPA2_PSK_WPA3_SAE: ap.auth = WIFI_AUTH_WPA2_WPA3_PSK; break;
      default: ap.auth = WIFI_AUTH_INVALID; break;
    }
}
static void scan_done(void *arg, void *) {
    auto *wifi = static_cast<WiFiClass *>(arg);
    auto *d = static_cast<BL602WiFiData *>(wifi->data);
    if (!lock(d)) return;
    if (!wifi->scan || !wifi->scan->running) { unlock(d); return; }
    if (wifi_mgmr_scan_ap_all(NULL, reinterpret_cast<uint32_t *>(wifi), scan_item) != 0)
        d->scan_failed = true;
    EventInfo info = {};
    info.wifi_scan_done.status = d->scan_failed ? 1 : 0;
    if (d->scan_failed) {
        // Preserve the scan object so even an allocation failure can complete
        // and notify the consumer without another allocation.
        for (unsigned i = 0; i < wifi->scan->count; ++i) free(wifi->scan->ap[i].ssid);
        free(wifi->scan->ap);
        wifi->scan->ap = NULL;
        wifi->scan->count = 0;
    }
    info.wifi_scan_done.number = wifi->scan->count;
    wifi->scan->running = false;
    unlock(d);
    emit(ARDUINO_EVENT_WIFI_SCAN_DONE, info);
}
int16_t WiFiClass::scanNetworks(bool async,bool hidden,bool passive,uint32_t ms,uint8_t channel) {
    if (channel > 14 || ms > 10000 || !enableSTA(true)) return WIFI_SCAN_FAILED;
    auto *d=BL_WIFI_DATA; if (!lock(d)) return WIFI_SCAN_FAILED;
    if (scan && scan->running) { unlock(d); return WIFI_SCAN_RUNNING; }
    scanDelete(); scanInit();
    if (!scan) { unlock(d); return WIFI_SCAN_FAILED; }
    d->scan_hidden=hidden; d->scan_passive=passive; d->scan_ms=ms; d->scan_channel=channel;
    d->scan_failed=false; d->pending_scan=true; scan->running=true;
    unlock(d); start_pending(this);
    if (async) return WIFI_SCAN_RUNNING;
    uint32_t started=millis();
    for (;;) {
        if (!lock(d)) return WIFI_SCAN_FAILED;
        bool running = scan && scan->running;
        int16_t result = scan && !running && !d->scan_failed ? scan->count : WIFI_SCAN_FAILED;
        unlock(d);
        if (!running) return result;
        // The SDK has no scan-abort API and stores one global completion
        // callback. Keep this scan marked running after the synchronous wait
        // expires so a later request cannot steal its eventual completion.
        if (millis()-started >= ms*20+5000) return WIFI_SCAN_FAILED;
        delay(10);
    }
}
