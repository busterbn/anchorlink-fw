#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "relays.h"

LOG_MODULE_REGISTER(relays, LOG_LEVEL_INF);

static const struct gpio_dt_spec relays[NUM_RELAYS] = {
	GPIO_DT_SPEC_GET(DT_ALIAS(relay0), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(relay1), gpios),
};

static bool relay_state[NUM_RELAYS];

int relays_init(void)
{
	for (int i = 0; i < NUM_RELAYS; i++) {
		if (!gpio_is_ready_dt(&relays[i])) {
			LOG_ERR("Relay %d GPIO not ready", i);
			return -ENODEV;
		}
		int err = gpio_pin_configure_dt(&relays[i], GPIO_OUTPUT_INACTIVE);
		if (err) {
			return err;
		}
	}

	LOG_INF("Relays initialized");
	return 0;
}

int relay_set(uint8_t idx, bool on)
{
	if (idx >= NUM_RELAYS) {
		return -EINVAL;
	}
	int err = gpio_pin_set_dt(&relays[idx], on);
	if (!err) {
		relay_state[idx] = on;
	}
	return err;
}

bool relay_get(uint8_t idx)
{
	if (idx >= NUM_RELAYS) {
		return false;
	}
	return relay_state[idx];
}

int relay_toggle(uint8_t idx)
{
	if (idx >= NUM_RELAYS) {
		return -EINVAL;
	}
	return relay_set(idx, !relay_state[idx]);
}
