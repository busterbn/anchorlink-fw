#include <zephyr/kernel.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/logging/log.h>

#include "power.h"

LOG_MODULE_REGISTER(power, LOG_LEVEL_INF);

#define ZEPHYR_USER DT_PATH(zephyr_user)

static const struct adc_dt_spec adc_current = ADC_DT_SPEC_GET_BY_IDX(ZEPHYR_USER, 0);
static const struct adc_dt_spec adc_battery = ADC_DT_SPEC_GET_BY_IDX(ZEPHYR_USER, 1);

static int16_t sample_buf;

static struct adc_sequence sequence = {
	.buffer = &sample_buf,
	.buffer_size = sizeof(sample_buf),
};

int power_init(void)
{
	int err;

	if (!adc_is_ready_dt(&adc_current)) {
		LOG_ERR("ADC current channel not ready");
		return -ENODEV;
	}

	if (!adc_is_ready_dt(&adc_battery)) {
		LOG_ERR("ADC battery channel not ready");
		return -ENODEV;
	}

	err = adc_channel_setup_dt(&adc_current);
	if (err) {
		LOG_ERR("ADC current channel setup failed: %d", err);
		return err;
	}

	err = adc_channel_setup_dt(&adc_battery);
	if (err) {
		LOG_ERR("ADC battery channel setup failed: %d", err);
		return err;
	}

	LOG_INF("Power ADC initialized");
	return 0;
}

static int read_channel_mv(const struct adc_dt_spec *spec, int32_t *millivolts)
{
	int err;

	err = adc_sequence_init_dt(spec, &sequence);
	if (err) {
		return err;
	}

	err = adc_read_dt(spec, &sequence);
	if (err) {
		return err;
	}

	*millivolts = (int32_t)sample_buf;
	err = adc_raw_to_millivolts_dt(spec, millivolts);
	if (err) {
		/* If conversion not supported, return raw value */
		*millivolts = (int32_t)sample_buf;
	}

	return 0;
}

int power_read_battery_mv(int32_t *millivolts)
{
	return read_channel_mv(&adc_battery, millivolts);
}

int power_read_current_ma(int32_t *milliamps)
{
	return read_channel_mv(&adc_current, milliamps);
}
