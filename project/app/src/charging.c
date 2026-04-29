#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>

#include "message_channel.h"
#include "sense.h"
#include "charging.h"

LOG_MODULE_REGISTER(charging, LOG_LEVEL_INF);

#define SAMPLE_INTERVAL_SECONDS  10
/* Hysteresis around the 12 V resting voltage of a lead-acid battery: above
 * 13.0 V we consider it charging, below 12.8 V we consider it not charging,
 * in between we keep the previous state. */
#define CHARGING_ON_V   13.0f
#define CHARGING_OFF_V  12.8f

#define NUM_BATTERIES 2

static bool charging_state[NUM_BATTERIES];
static bool charging_known[NUM_BATTERIES];

static const enum sense_channel bat_channel[NUM_BATTERIES] = {
	SENSE_VBAT,
	SENSE_VBAT2,
};

static void charging_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(charging_work, charging_work_fn);

bool charging_get(uint8_t idx)
{
	return idx < NUM_BATTERIES && charging_state[idx];
}

static bool decide(bool prev, bool known, float volts)
{
	if (volts > CHARGING_ON_V) {
		return true;
	}
	if (volts < CHARGING_OFF_V) {
		return false;
	}
	return known ? prev : false;
}

static void publish_charging(uint8_t idx, bool state)
{
	struct publish_event ev = {
		.type = PUB_CHARGING,
		.battery = idx,
		.charging = state,
	};
	zbus_chan_pub(&PUBLISH_CHAN, &ev, K_SECONDS(1));
}

static void measure_one(uint8_t idx)
{
	float v = 0.0f;
	if (sense_read_v(bat_channel[idx], &v)) {
		return;
	}

	bool prev = charging_state[idx];
	bool known = charging_known[idx];
	bool now = decide(prev, known, v);

	if (!known || now != prev) {
		charging_state[idx] = now;
		charging_known[idx] = true;
		LOG_INF("bat%u %s (%d mV)",
			(unsigned)(idx + 1),
			now ? "charging" : "not charging",
			(int)(v * 1000.0f));
		publish_charging(idx, now);
	}
}

static void charging_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	for (uint8_t i = 0; i < NUM_BATTERIES; i++) {
		measure_one(i);
	}
	k_work_schedule(&charging_work, K_SECONDS(SAMPLE_INTERVAL_SECONDS));
}

void charging_init(void)
{
	/* First sample shortly after boot, after sense init has settled. */
	k_work_schedule(&charging_work, K_SECONDS(2));
}
