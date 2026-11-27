/* BL602 hardware-timer based Arduino clock. */
#include <ArduinoPrivate.h>
#include <bl_timer.h>
#include <reent.h>
#include <sys/time.h>

/*
 * newlib's time() (and other libc time functions) call _gettimeofday_r(), which in the riscv32-esp-elf newlib is a
 * stub that fails: time() returned -1 even after settimeofday() (e.g. ESPHome's time sync logged 1969-12-31 23:59:59).
 * Route it to the LibreTiny clock behind gettimeofday()/settimeofday() (cores/common/arduino/src/posix/time.c).
 */
int _gettimeofday_r(struct _reent *r, struct timeval *tv, void *tz) {
	(void)r;
	return gettimeofday(tv, tz);
}

unsigned long millis(void) {
	return (unsigned long)(bl_timer_now_us64() / 1000ULL);
}

unsigned long micros(void) {
	return (unsigned long)bl_timer_now_us();
}

void delayMicroseconds(unsigned int us) {
	if (us)
		bl_timer_delay_us((uint32_t)us);
}
