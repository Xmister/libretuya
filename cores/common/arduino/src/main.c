/* Copyright (c) Kuba Szczodrzyński 2023-02-27. */

#include <Arduino.h>

#if !defined(LT_BL60X)
int main() {
	// initialize Arduino framework
	lt_init_arduino();
	// start the main task and OS kernel
	if (!startMainTask()) {
		LT_F("Couldn't start the main task");
		return 1;
	}
	return 0;
}
#endif

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
