#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>

#include "message_channel.h"
#include "relays.h"
#include "sense.h"
#include "buttons.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

ZBUS_SUBSCRIBER_DEFINE(main_sub, 4);

static void publish_relay_state(uint8_t idx)
{
	struct publish_event ev = {
		.type = PUB_RELAY_STATE,
		.relay = idx,
		.state = relay_get(idx),
	};
	zbus_chan_pub(&PUBLISH_CHAN, &ev, K_SECONDS(1));
}

static void handle_toggle(uint8_t idx)
{
	if (idx >= NUM_RELAYS) {
		return;
	}
	relay_toggle(idx);
	LOG_INF("Relay %d -> %s", idx, relay_get(idx) ? "ON" : "OFF");
	publish_relay_state(idx);
}

static void handle_report_bat(void)
{
	float v1 = 0.0f, v2 = 0.0f;
	sense_read_v(SENSE_VBAT, &v1);
	sense_read_v(SENSE_VBAT2, &v2);

	struct publish_event ev = {
		.type = PUB_BAT_REPORT,
		.bat1_v = v1,
		.bat2_v = v2,
	};
	zbus_chan_pub(&PUBLISH_CHAN, &ev, K_SECONDS(1));
}

static void on_button_pressed(uint8_t idx)
{
	switch (idx) {
	case 0:
		LOG_INF("BTN0 pressed");
		break;
	case 1:
		LOG_INF("BTN1 pressed");
		handle_toggle(0);
		break;
	case 2:
		LOG_INF("BTN2 pressed");
		handle_toggle(1);
		break;
	}
}

int main(void)
{
	const struct zbus_channel *chan;

	LOG_INF("Boat Monitor starting");

	relays_init();
	sense_init();
	buttons_init(on_button_pressed);

	while (!zbus_sub_wait(&main_sub, &chan, K_FOREVER)) {
		if (chan != &CMD_CHAN) {
			continue;
		}

		struct command cmd;
		if (zbus_chan_read(&CMD_CHAN, &cmd, K_SECONDS(1))) {
			continue;
		}

		switch (cmd.action) {
		case CMD_TOGGLE_RELAY:
			handle_toggle(cmd.relay);
			break;
		case CMD_REPORT_BAT:
			handle_report_bat();
			break;
		}
	}

	return 0;
}
