#pragma once
#error "Read by the LibreTiny builder, not included directly"
#define LT_HAS_FREERTOS	 1
#define LT_HAS_LWIP		 1
#define LT_HAS_LWIP2	 1
#define LT_HAS_MBEDTLS	 1
#define LT_HAS_PRINTF	 1
#define LT_HAS_FLASH	 0
#define LT_HAS_OTA		 0
#define LT_HW_WIFI		 0
#define LT_HW_BLE		 1
#define LT_HW_BT		 0
#define LT_HW_DEEP_SLEEP 0
#define LT_HW_WATCHDOG	 0
#define LT_HEAP_FUNC	 xPortGetFreeHeapSize
#define LT_REALLOC_FUNC	 pvPortRealloc
