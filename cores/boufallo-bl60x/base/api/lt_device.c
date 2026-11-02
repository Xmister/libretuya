#include <libretiny.h>
#include <sdk_private.h>

void lt_get_device_mac(uint8_t *mac) {
    if (bl_wifi_mac_addr_get(mac) != 0) memset(mac, 0, 6);
}
void lt_reboot(void) {
    GLB_SW_POR_Reset();
    for (;;) {}
}
