/* BL602 GPIO interrupt support. CHANGE is deliberately unsupported: the
 * controller selects one polarity and software re-arming can lose edges. */
#include <ArduinoPrivate.h>
#include <bl_gpio.h>
#include <bl_irq.h>
#include <bl602_glb.h>

typedef struct {
    voidFuncPtrParam callback;
    void *param;
} bl602_irq_slot;
static bl602_irq_slot irq_slots[PINS_GPIO_MAX + 1];
static bool irq_registered;

static void bl602_gpio_irq(void) {
    for (unsigned gpio = 0; gpio <= PINS_GPIO_MAX; gpio++) {
        bl602_irq_slot *slot = &irq_slots[gpio];
        if (!slot->callback || GLB_Get_GPIO_IntStatus((GLB_GPIO_Type)gpio) != SET) continue;
        GLB_Clr_GPIO_IntStatus((GLB_GPIO_Type)gpio);
        if (slot->param) slot->callback(slot->param);
        else ((voidFuncPtr)slot->callback)();
    }
}

void attachInterruptParam(pin_size_t number, voidFuncPtrParam callback, PinStatus mode, void *param) {
    pinCheckGetData(number, PIN_IRQ, );
    (void)data;
    if (!callback || pin->gpio > PINS_GPIO_MAX) return;
    GLB_GPIO_INT_TRIG_Type trigger;
    switch (mode) {
        case RISING: trigger = GLB_GPIO_INT_TRIG_POS_PULSE; break;
        case FALLING: trigger = GLB_GPIO_INT_TRIG_NEG_PULSE; break;
        case HIGH: trigger = GLB_GPIO_INT_TRIG_POS_LEVEL; break;
        case LOW: trigger = GLB_GPIO_INT_TRIG_NEG_LEVEL; break;
        default:
            detachInterrupt(number);
            LT_E("BL602 GPIO interrupt mode %d is unsupported", mode);
            return;
    }
    bl_gpio_intmask(pin->gpio, 1);
    GLB_Clr_GPIO_IntStatus((GLB_GPIO_Type)pin->gpio);
    pinDisable(pin, PIN_IRQ);
    irq_slots[pin->gpio].callback = callback;
    irq_slots[pin->gpio].param = param;
    if (GLB_Set_GPIO_IntMod((GLB_GPIO_Type)pin->gpio, GLB_GPIO_INT_CONTROL_ASYNC, trigger) != SUCCESS) {
        irq_slots[pin->gpio].callback = NULL;
        return;
    }
    pinEnable(pin, PIN_IRQ);
    if (!irq_registered) {
        bl_irq_register(GPIO_INT0_IRQn, (void *)bl602_gpio_irq);
        bl_irq_enable(GPIO_INT0_IRQn);
        irq_registered = true;
    }
    bl_gpio_intmask(pin->gpio, 0);
}

void detachInterrupt(pin_size_t number) {
    pinCheckGetData(number, PIN_IRQ, );
    (void)data;
    if (pin->gpio > PINS_GPIO_MAX) return;
    bl_gpio_intmask(pin->gpio, 1);
    GLB_Clr_GPIO_IntStatus((GLB_GPIO_Type)pin->gpio);
    irq_slots[pin->gpio].callback = NULL;
    irq_slots[pin->gpio].param = NULL;
    pinDisable(pin, PIN_IRQ);
}
