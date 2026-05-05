#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>

#include "message_channel.h"
#include "sense.h"
#include "battery_monitor.h"

LOG_MODULE_REGISTER(battery_monitor, LOG_LEVEL_INF);

#define SAMPLE_INTERVAL_SECONDS  60
#define DELTA_THRESHOLD_V        0.10f
#define NUM_BATTERIES            2

static const enum sense_channel bat_channel[NUM_BATTERIES] = {
	SENSE_VBAT,
	SENSE_VBAT2,
};

static float last_sent_v[NUM_BATTERIES];
static bool  has_baseline;

static void battery_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(battery_work, battery_work_fn);

static void publish(float v1, float v2)
{
	struct publish_event ev = {
		.type   = PUB_BAT_REPORT,
		.bat1_v = v1,
		.bat2_v = v2,
	};
	zbus_chan_pub(&PUBLISH_CHAN, &ev, K_SECONDS(1));
}

static int read_both(float v[NUM_BATTERIES])
{
	for (int i = 0; i < NUM_BATTERIES; i++) {
		if (sense_read_v(bat_channel[i], &v[i])) {
			return -1;
		}
	}
	return 0;
}

static void battery_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	float v[NUM_BATTERIES] = {0};
	if (read_both(v) == 0) {
		bool changed = !has_baseline;
		for (int i = 0; i < NUM_BATTERIES; i++) {
			float d = v[i] - last_sent_v[i];
			if (d < 0) { d = -d; }
			if (d >= DELTA_THRESHOLD_V) {
				changed = true;
			}
		}
		if (changed) {
			for (int i = 0; i < NUM_BATTERIES; i++) {
				last_sent_v[i] = v[i];
			}
			has_baseline = true;
			LOG_INF("bat report %d / %d mV",
				(int)(v[0] * 1000.0f), (int)(v[1] * 1000.0f));
			publish(v[0], v[1]);
		}
	}

	k_work_schedule(&battery_work, K_SECONDS(SAMPLE_INTERVAL_SECONDS));
}

void battery_monitor_force_report(void)
{
	float v[NUM_BATTERIES] = {0};
	if (read_both(v)) {
		return;
	}
	for (int i = 0; i < NUM_BATTERIES; i++) {
		last_sent_v[i] = v[i];
	}
	has_baseline = true;
	publish(v[0], v[1]);
}

void battery_monitor_init(void)
{
	/* First sample shortly after boot, after sense init has settled. */
	k_work_schedule(&battery_work, K_SECONDS(2));
}
