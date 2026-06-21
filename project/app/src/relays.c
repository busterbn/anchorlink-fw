#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include "relays.h"

LOG_MODULE_REGISTER(relays, LOG_LEVEL_INF);

#define RELAY_STATE_KEY "relays/state"

static const struct gpio_dt_spec relays[NUM_RELAYS] = {
	GPIO_DT_SPEC_GET(DT_ALIAS(relay0), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(relay1), gpios),
};

static bool relay_state[NUM_RELAYS];

/* Both relay states are packed into one byte (bit i = relay i) so a save is a
 * single NVS write. */
static int relays_settings_set(const char *name, size_t len,
			       settings_read_cb read_cb, void *cb_arg)
{
	if (settings_name_steq(name, "state", NULL)) {
		uint8_t mask = 0;
		if (read_cb(cb_arg, &mask, sizeof(mask)) > 0) {
			for (int i = 0; i < NUM_RELAYS; i++) {
				relay_state[i] = (mask >> i) & 1;
			}
		}
		return 0;
	}
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(relays, "relays", NULL, relays_settings_set, NULL, NULL);

static void relays_save(void)
{
	uint8_t mask = 0;
	for (int i = 0; i < NUM_RELAYS; i++) {
		if (relay_state[i]) {
			mask |= BIT(i);
		}
	}
	int err = settings_save_one(RELAY_STATE_KEY, &mask, sizeof(mask));
	if (err) {
		LOG_ERR("Failed to save relay state: %d", err);
	}
}

int relays_init(void)
{
	/* Restore the saved relay state before driving the outputs, so the
	 * relays come back in the same position after a reboot. */
	settings_subsys_init();
	settings_load_subtree("relays");

	for (int i = 0; i < NUM_RELAYS; i++) {
		if (!gpio_is_ready_dt(&relays[i])) {
			LOG_ERR("Relay %d GPIO not ready", i);
			return -ENODEV;
		}
		int err = gpio_pin_configure_dt(&relays[i],
			relay_state[i] ? GPIO_OUTPUT_ACTIVE : GPIO_OUTPUT_INACTIVE);
		if (err) {
			return err;
		}
	}

	LOG_INF("Relays initialized (relay0=%d relay1=%d)",
		relay_state[0], relay_state[1]);
	return 0;
}

int relay_set(uint8_t idx, bool on)
{
	if (idx >= NUM_RELAYS) {
		return -EINVAL;
	}
	int err = gpio_pin_set_dt(&relays[idx], on);
	if (!err && relay_state[idx] != on) {
		relay_state[idx] = on;
		relays_save();
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
