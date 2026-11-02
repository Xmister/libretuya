/* BL602 hardware-timer based Arduino clock. */
#include <ArduinoPrivate.h>
#include <bl_timer.h>

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
