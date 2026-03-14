#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>

#include "message_channel.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

ZBUS_SUBSCRIBER_DEFINE(main_sub, 4);
ZBUS_CHAN_ADD_OBS(NETWORK_CHAN, main_sub, 0);

int main(void)
{
	const struct zbus_channel *chan;
	enum network_status status;

	LOG_INF("Hello world!");

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
