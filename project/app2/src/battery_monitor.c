#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>

#include "message_channel.h"
#include "sense.h"
#include "battery_monitor.h"

LOG_MODULE_REGISTER(battery_monitor, LOG_LEVEL_INF);

#define NUM_BATTERIES 2

static const enum sense_channel bat_channel[NUM_BATTERIES] = {
	SENSE_VBAT,
	SENSE_VBAT2,
};

void battery_monitor_force_report(void)
{
	float v[NUM_BATTERIES] = {0};
	for (int i = 0; i < NUM_BATTERIES; i++) {
		if (sense_read_v(bat_channel[i], &v[i])) {
			return;
		}
	}

	LOG_INF("bat report %d / %d mV",
		(int)(v[0] * 1000.0f), (int)(v[1] * 1000.0f));

	struct publish_event ev = {
		.type   = PUB_BAT_REPORT,
		.bat1_v = v[0],
		.bat2_v = v[1],
	};
	zbus_chan_pub(&PUBLISH_CHAN, &ev, K_SECONDS(1));
}
