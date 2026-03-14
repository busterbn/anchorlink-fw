#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>

#include "message_channel.h"
#include "relays.h"
#include "power.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

ZBUS_SUBSCRIBER_DEFINE(main_sub, 4);
ZBUS_CHAN_ADD_OBS(NETWORK_CHAN, main_sub, 0);

int main(void)
{
	const struct zbus_channel *chan;
	enum network_status status;

	LOG_INF("Hello world!");

	relays_init();
	power_init();

	/* Test: read ADC channels */
	int32_t battery_mv, current_mv;

	for (int i = 0; i < 5; i++) {
		if (!power_read_battery_mv(&battery_mv)) {
			LOG_INF("Battery: %d mV", battery_mv);
		}
		if (!power_read_current_ma(&current_mv)) {
			LOG_INF("Current sense: %d mV", current_mv);
		}
		k_sleep(K_SECONDS(2));
	}

	/* Test: cycle through each relay */
	for (int i = 0; i < NUM_RELAYS; i++) {
		LOG_INF("Relay %d ON", i);
		relay_set(i, true);
		k_sleep(K_SECONDS(2));
		LOG_INF("Relay %d OFF", i);
		relay_set(i, false);
		k_sleep(K_SECONDS(1));
	}

	LOG_INF("All relays ON");
	relays_set_all(0x1F);
	k_sleep(K_SECONDS(3));
	LOG_INF("All relays OFF");
	relays_set_all(0x00);

	/* Wait for network connected event */
	while (!zbus_sub_wait(&main_sub, &chan, K_FOREVER)) {
		if (chan == &NETWORK_CHAN) {
			zbus_chan_read(&NETWORK_CHAN, &status, K_SECONDS(1));
			if (status == NETWORK_CONNECTED) {
				break;
			}
		}
	}

	/* Wait for MQTT connection to be established */
	k_sleep(K_SECONDS(15));

	struct payload payload = { 0 };

	snprintk(payload.string, sizeof(payload.string), "Hello World");

	int err = zbus_chan_pub(&PAYLOAD_CHAN, &payload, K_SECONDS(1));
	if (err) {
		LOG_ERR("Failed to publish: %d", err);
	} else {
		LOG_INF("Hello World sent");
	}

	return 0;
}
