/* Optional diagnostic wrappers for the pinned BL602 controller library. */
#ifdef LT_BL602_BLE_INIT_TRACE
#include <libretiny.h>
#include <stdint.h>

extern void __real_rfc_init(uint32_t xtal_hz);
extern void __real_rwip_init(uint32_t error);

void __wrap_rfc_init(uint32_t xtal_hz) {
    LT_DM(BLE, "[BLETRACE] rfc_init enter %lu", (unsigned long) xtal_hz);
    __real_rfc_init(xtal_hz);
    LT_DM(BLE, "[BLETRACE] rfc_init return");
}

void __wrap_rwip_init(uint32_t error) {
    LT_DM(BLE, "[BLETRACE] rwip_init enter %lu", (unsigned long) error);
    __real_rwip_init(error);
    LT_DM(BLE, "[BLETRACE] rwip_init return");
}

/* These signatures follow the calling convention in the pinned BL602 SDK
 * controller ELF. They are diagnostics only, not a public SDK interface. */
#define TRACE_VOID(name) \
    extern void __real_##name(void); \
    void __wrap_##name(void) { \
        LT_DM(BLE, "[BLETRACE] " #name " enter"); \
        __real_##name(); \
        LT_DM(BLE, "[BLETRACE] " #name " return"); \
    }
TRACE_VOID(ble_ke_init)
TRACE_VOID(hci_init)
TRACE_VOID(rwble_init)

extern void __real_ble_ke_mem_init(uint32_t type, void *memory, uint32_t size);
void __wrap_ble_ke_mem_init(uint32_t type, void *memory, uint32_t size) {
    LT_DM(BLE, "[BLETRACE] ke_mem_init enter %lu %p %lu", (unsigned long)type, memory, (unsigned long)size);
    __real_ble_ke_mem_init(type, memory, size);
    LT_DM(BLE, "[BLETRACE] ke_mem_init return");
}

extern void __real_ble_rf_init(void *config);
void __wrap_ble_rf_init(void *config) {
    LT_DM(BLE, "[BLETRACE] ble_rf_init enter");
    __real_ble_rf_init(config);
    LT_DM(BLE, "[BLETRACE] ble_rf_init return");
}

#define TRACE_U32(name) \
    extern void __real_##name(uint32_t value); \
    void __wrap_##name(uint32_t value) { \
        LT_DM(BLE, "[BLETRACE] " #name " enter %lu", (unsigned long)value); \
        __real_##name(value); \
        LT_DM(BLE, "[BLETRACE] " #name " return"); \
    }
TRACE_U32(ecc_init)
TRACE_U32(ea_init)
#endif
