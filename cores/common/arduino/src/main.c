/* Copyright (c) Kuba Szczodrzyński 2023-02-27. */

#include <Arduino.h>

#if defined(LT_BL60X)
int lt_bl602_application_main(void) {
#else
int main() {
#endif
	// initialize Arduino framework
#if defined(LT_BL60X)
	if (lt_init_arduino) lt_init_arduino();
#else
	lt_init_arduino();
#endif
	// start the main task and OS kernel
	if (!startMainTask()) {
		LT_F("Couldn't start the main task");
		return 1;
	}
	return 0;
}

/**
 * @brief Main setup() and loop() task.
 * Not to be called directly.
 */
void mainTask(const void *arg) {
	setup();

	for (;;) {
		loop();
		yield();
	}
}
