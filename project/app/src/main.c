#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/drivers/gpio.h>

#include "message_channel.h"
#include "relays.h"
#include "sense.h"
#include "buttons.h"
#include "battery_monitor.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* P0.14: kept permanently active from boot. */
static const struct gpio_dt_spec ignition_en = GPIO_DT_SPEC_GET(DT_NODELABEL(ignition_en), gpios);

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
	battery_monitor_force_report();
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

int main(void)
{
	const struct zbus_channel *chan;

	LOG_INF("Boat Monitor starting");
	LOG_INF("Firmware version: %s", CONFIG_MEMFAULT_NCS_FW_VERSION);

	relays_init();
	sense_init();
	buttons_init(on_button_pressed, on_button_long_pressed);

	/* P0.14 stays permanently active. Done after sense_init(), which would
	 * otherwise leave the ignition_en pin inactive. */
	gpio_pin_configure_dt(&ignition_en, GPIO_OUTPUT_ACTIVE);

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
		case CMD_REBOOT:
			LOG_INF("Reboot requested via MQTT");
			k_sleep(K_SECONDS(2));
			sys_reboot(SYS_REBOOT_COLD);
			break;
		default:
			/* Other commands handled by other subscribers (gps). */
			break;
		}
	}

	return 0;
}
