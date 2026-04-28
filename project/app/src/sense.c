#include <zephyr/kernel.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "sense.h"
#include "relays.h"

LOG_MODULE_REGISTER(sense, LOG_LEVEL_INF);

#define ZEPHYR_USER DT_PATH(zephyr_user)

/* Channel order matches enum sense_channel:
 * 0=REL0_SENSE, 1=REL1_SENSE, 2=VBAT_SENSE, 3=VBAT2_SENSE, 4=IGNITION_SENSE
 */
static const struct adc_dt_spec adc_channels[SENSE_COUNT] = {
	ADC_DT_SPEC_GET_BY_IDX(ZEPHYR_USER, 0),
	ADC_DT_SPEC_GET_BY_IDX(ZEPHYR_USER, 1),
	ADC_DT_SPEC_GET_BY_IDX(ZEPHYR_USER, 2),
	ADC_DT_SPEC_GET_BY_IDX(ZEPHYR_USER, 3),
	ADC_DT_SPEC_GET_BY_IDX(ZEPHYR_USER, 4),
};

/* Dedicated EN pins for VBAT, VBAT2, IGNITION sensing.
 * REL0_SENSE/REL1_SENSE share their EN with the relay drive (relay_set). */
static const struct gpio_dt_spec vbat_en = GPIO_DT_SPEC_GET(DT_NODELABEL(vbat_en), gpios);
static const struct gpio_dt_spec vbat2_en = GPIO_DT_SPEC_GET(DT_NODELABEL(vbat2_en), gpios);
static const struct gpio_dt_spec ignition_en = GPIO_DT_SPEC_GET(DT_NODELABEL(ignition_en), gpios);

static int16_t sample_buf;
static struct adc_sequence sequence = {
	.buffer = &sample_buf,
	.buffer_size = sizeof(sample_buf),
};

int sense_init(void)
{
	const struct gpio_dt_spec *en_pins[] = { &vbat_en, &vbat2_en, &ignition_en };

	for (int i = 0; i < SENSE_COUNT; i++) {
		if (!adc_is_ready_dt(&adc_channels[i])) {
			LOG_ERR("ADC channel %d not ready", i);
			return -ENODEV;
		}
		int err = adc_channel_setup_dt(&adc_channels[i]);
		if (err) {
			LOG_ERR("ADC channel %d setup failed: %d", i, err);
			return err;
		}
	}

	for (int i = 0; i < ARRAY_SIZE(en_pins); i++) {
		if (!gpio_is_ready_dt(en_pins[i])) {
			LOG_ERR("Sense EN pin %d not ready", i);
			return -ENODEV;
		}
		int err = gpio_pin_configure_dt(en_pins[i], GPIO_OUTPUT_INACTIVE);
		if (err) {
			return err;
		}
	}

	LOG_INF("Sense initialized");
	return 0;
}

/* Returns the EN pin for a channel, or NULL if EN is shared with a relay. */
static const struct gpio_dt_spec *en_for(enum sense_channel ch)
{
	switch (ch) {
	case SENSE_VBAT:     return &vbat_en;
	case SENSE_VBAT2:    return &vbat2_en;
	case SENSE_IGNITION: return &ignition_en;
	default:             return NULL;
	}
}

int sense_read_mv(enum sense_channel ch, int32_t *millivolts)
{
	if (ch >= SENSE_COUNT || !millivolts) {
		return -EINVAL;
	}

	const struct gpio_dt_spec *en = en_for(ch);
	bool toggled_en = false;

	if (en) {
		gpio_pin_set_dt(en, 1);
		toggled_en = true;
		k_msleep(2);
	}
	/* For REL0_SENSE / REL1_SENSE the EN is the relay drive itself —
	 * we measure whatever the current relay state implies. */

	int err = adc_sequence_init_dt(&adc_channels[ch], &sequence);
	if (!err) {
		err = adc_read_dt(&adc_channels[ch], &sequence);
	}

	if (toggled_en) {
		gpio_pin_set_dt(en, 0);
	}

	if (err) {
		return err;
	}

	int32_t mv = (int32_t)sample_buf;
	if (adc_raw_to_millivolts_dt(&adc_channels[ch], &mv) != 0) {
		mv = (int32_t)sample_buf;
	}
	*millivolts = mv;
	return 0;
}

int sense_read_v(enum sense_channel ch, float *volts)
{
	int32_t mv = 0;
	int err = sense_read_mv(ch, &mv);
	if (err) {
		return err;
	}
	*volts = mv / 1000.0f;
	return 0;
}
