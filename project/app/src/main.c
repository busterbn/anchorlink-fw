#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>

#include <date_time.h>

#include "message_channel.h"
#include "relays.h"
#include "sense.h"
#include "buttons.h"
#include "charging.h"

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

static void on_button_long_pressed(uint8_t idx)
{
	if (idx != 0) {
		return;
	}
	LOG_INF("BTN0 long press -> pair");
	struct publish_event ev = { .type = PUB_PAIR };
	zbus_chan_pub(&PUBLISH_CHAN, &ev, K_SECONDS(1));
}

static void hourly_bat_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(hourly_bat_work, hourly_bat_work_fn);

static void hourly_bat_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	int64_t now_ms;
	int err = date_time_now(&now_ms);
	if (err) {
		LOG_DBG("date_time not ready (%d), retrying in 60s", err);
		k_work_reschedule(&hourly_bat_work, K_SECONDS(60));
		return;
	}

	int64_t now_s = now_ms / 1000;
	int64_t past_hour = now_s % 3600;

	/* If we are within the first 5 s of an hour, treat it as "the hour"
	 * and report; otherwise just sleep until the next one. */
	if (past_hour < 5) {
		LOG_INF("Hourly battery report");
		struct command cmd = { .action = CMD_REPORT_BAT };
		zbus_chan_pub(&CMD_CHAN, &cmd, K_SECONDS(1));
	}

	int64_t to_next = 3600 - past_hour;
	k_work_reschedule(&hourly_bat_work, K_SECONDS(to_next));
}

int main(void)
{
	const struct zbus_channel *chan;

	LOG_INF("Boat Monitor starting");

	relays_init();
	sense_init();
	buttons_init(on_button_pressed, on_button_long_pressed);
	charging_init();

	k_work_reschedule(&hourly_bat_work, K_SECONDS(60));

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
		default:
			/* Other commands handled by other subscribers (gps). */
			break;
		}
	}

	return 0;
}
