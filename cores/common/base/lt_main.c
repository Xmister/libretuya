/* Copyright (c) Kuba Szczodrzyński 2022-06-19. */

#include <libretiny.h>

#include <fal.h>

fal_partition_t fal_root_part = NULL;

// Initialize C library
void __libc_init_array(void);
// Main app entrypoint
int main(void);
#if defined(LT_BL60X)
int lt_bl602_application_main(void);
void lt_bl602_early_init(void);
#endif

int lt_main(void) {
	// early initialize the family and variant
#if defined(LT_BL60X)
	lt_bl602_early_init();
	if (lt_init_variant) lt_init_variant();
#else
	lt_init_family();
	lt_init_variant();
#endif
	// print a startup banner
	LT_BANNER();
	// initialize C library
#if !defined(LT_BL60X)
	__libc_init_array();
#endif
	// inform about the reset reason
	LT_I("Reset reason: %s", lt_get_reboot_reason_name(0));
	// initialize FAL
	fal_init();
	// provide root partition
	fal_root_part = (fal_partition_t)fal_partition_find("root");

	// run the application
#if defined(LT_BL60X)
	return lt_bl602_application_main();
#else
	return main();
#endif
}
