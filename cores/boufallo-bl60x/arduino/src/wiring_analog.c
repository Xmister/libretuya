/* BL602 hardware PWM implementation for the common Arduino API. */
#include <ArduinoPrivate.h>
#include <bl_pwm.h>

#define BL602_PWM_CHANNELS 5
#define BL602_PWM_FREQ_MIN 2000U
#define BL602_PWM_FREQ_MAX 800000U

static int8_t pwm_pin[BL602_PWM_CHANNELS] = {-1, -1, -1, -1, -1};
static uint32_t pwm_freq[BL602_PWM_CHANNELS];

static uint32_t pwm_frequency() {
	uint32_t frequency = _analogWritePeriod > 0 ?
		1000000U / (uint32_t)_analogWritePeriod : 5000U;
	if (frequency < BL602_PWM_FREQ_MIN)
		return BL602_PWM_FREQ_MIN;
	if (frequency > BL602_PWM_FREQ_MAX)
		return BL602_PWM_FREQ_MAX;
	return frequency;
}

void analogWrite(pin_size_t pin_number, int value) {
	PinInfo *pin = pinInfo(pin_number);
	if (!pin || !pinSupported(pin, PIN_PWM))
		return;

	const uint8_t channel = (uint8_t)(pin->gpio % BL602_PWM_CHANNELS);
	if (pwm_pin[channel] >= 0 && pwm_pin[channel] != pin_number) {
		bl_pwm_stop(channel);
		pinDisable(pinInfo(pwm_pin[channel]), PIN_PWM);
		pwm_pin[channel] = -1;
	}

	uint8_t resolution = (uint8_t)_analogWriteResolution;
	if (resolution < 1)
		resolution = 1;
	if (resolution > 16)
		resolution = 16;
	const uint32_t max_duty = (1U << resolution) - 1;
	if (value < 0)
		value = 0;
	if ((uint32_t)value > max_duty)
		value = (int)max_duty;
	const float duty = (float)value * 100.0f / (float)max_duty;
	const uint32_t frequency = pwm_frequency();

	if (pwm_pin[channel] != pin_number) {
		if (bl_pwm_init(channel, pin->gpio, frequency) != 0)
			return;
		pwm_pin[channel] = pin_number;
		pwm_freq[channel] = frequency;
		pinEnable(pin, PIN_PWM);
	} else if (pwm_freq[channel] != frequency) {
		bl_pwm_set_freq(channel, frequency);
		pwm_freq[channel] = frequency;
	}

	bl_pwm_set_duty(channel, duty);
	bl_pwm_start(channel);
}
