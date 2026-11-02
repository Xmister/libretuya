/* The BL602 SDK calls main() from an already running FreeRTOS task. */
#include <ArduinoPrivate.h>
extern "C" {
#include <FreeRTOS.h>
#include <task.h>
}

static void bl602_main_task(void *argument) {
	mainTask(argument);
	vTaskDelete(nullptr);
}

extern "C" bool startMainTask(void) {
	/* xTaskCreate stack depth is in StackType_t units, not bytes. */
	constexpr uint32_t stack_bytes = 8192;
	const auto stack_words		   = stack_bytes / sizeof(StackType_t);
	return xTaskCreate(bl602_main_task, "arduino", stack_words, nullptr, 5, nullptr) == pdPASS;
}
