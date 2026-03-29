/*
 * Main application — initializes hardware and handles relay commands.
 * Listens on CMD_CHAN for set_relay commands, toggles relays,
 * and triggers a state publish (on-change).
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>

#include "message_channel.h"
#include "relays.h"
#include "power.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

ZBUS_SUBSCRIBER_DEFINE(main_sub, 4);

static void trigger_state_publish(void)
{
	int not_used = 0;
	zbus_chan_pub(&TRIGGER_CHAN, &not_used, K_SECONDS(1));
}

int main(void)
{
	const struct zbus_channel *chan;

	LOG_INF("Boat Monitor starting");

	relays_init();
	power_init();

	while (!zbus_sub_wait(&main_sub, &chan, K_FOREVER)) {
		if (&CMD_CHAN == chan) {
			struct command cmd;
			int err = zbus_chan_read(&CMD_CHAN, &cmd, K_SECONDS(1));

			if (err) {
				continue;
			}

			if (cmd.action == CMD_SET_RELAY) {
				if (cmd.relay < NUM_RELAYS) {
					relay_set(cmd.relay, cmd.state);
					LOG_INF("Relay %d set to %s",
						cmd.relay + 1,
						cmd.state ? "ON" : "OFF");
					trigger_state_publish();
				} else {
					LOG_WRN("Invalid relay index: %d", cmd.relay);
				}
			}
		}
	}

	return 0;
}
