/* Experimental BL602 digital GPIO implementation. */
#include <ArduinoPrivate.h>
#include <bl_gpio.h>
#include <bl602_glb.h>

void pinMode(pin_size_t pinNumber, PinMode mode) {
    pinCheckGetData(pinNumber, PIN_GPIO, );
    if (!data) return;
    int result;
    switch (mode) {
    case INPUT: result = bl_gpio_enable_input(pin->gpio, 0, 0); break;
    case INPUT_PULLUP: result = bl_gpio_enable_input(pin->gpio, 1, 0); break;
    case INPUT_PULLDOWN: result = bl_gpio_enable_input(pin->gpio, 0, 1); break;
    case OUTPUT:
        result = bl_gpio_enable_output(pin->gpio, 0, 0);
        if (!result) result = GLB_GPIO_INPUT_Enable((GLB_GPIO_Type)pin->gpio);
        break;
    default: return;
    }
    if (result) return;
    data->gpioMode = mode;
    pinEnable(pin, PIN_GPIO);
}
void digitalWrite(pin_size_t pinNumber, PinStatus status) {
    pinCheckGetData(pinNumber, PIN_GPIO, );
    if (!data || !pinEnabled(pin, PIN_GPIO)) return;
    if (data->gpioMode == OUTPUT) {
        (void)bl_gpio_output_set(pin->gpio, status != LOW);
    } else {
        /* Arduino's digitalWrite on an input changes its pull-up. */
        pinMode(pinNumber, status == LOW ? INPUT : INPUT_PULLUP);
    }
}
PinStatus digitalRead(pin_size_t pinNumber) {
    pinCheckGetData(pinNumber, PIN_GPIO, LOW);
    if (!data || !pinEnabled(pin, PIN_GPIO)) return LOW;
    return bl_gpio_input_get_value(pin->gpio) > 0 ? HIGH : LOW;
}
void pinRemoveMode(PinInfo *pin, uint32_t mask) {
    if (!pin) return;
    if ((mask & PIN_IRQ) && pinEnabled(pin, PIN_IRQ)) detachInterrupt(pin->gpio);
    if (!(mask & PIN_GPIO) || !pinEnabled(pin, PIN_GPIO)) return;
    if (bl_gpio_enable_input(pin->gpio, 0, 0) == 0) pinDisable(pin, PIN_GPIO);
}
