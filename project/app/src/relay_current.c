/*
 * Per-relay current measurement.
 *
 * The 10 mOhm shunt sits between battery 1 and the relay output. The shunt's
 * high side is V_bat1; the low side is what REL{0,1}_SENSE measures via its
 * own divider. With the relay closed:
 *
 *     I = (V_bat1 - V_relayX) / R_shunt
 *
 * Both ends are sensed through hardware dividers; sense_read_v() already
 * applies the divider scale, so we work in actual volts here.
 *
 * Sampling cadence: every 10 s while a relay is on. Once per hour (UTC,
 * aligned to top of hour) we publish the running average and the most recent
 * sample on {imei}/relay{x}/current_h, then reset the accumulator.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>

#include <date_time.h>

#include "message_channel.h"
#include "relays.h"
#include "sense.h"
#include "relay_current.h"

LOG_MODULE_REGISTER(relay_current, LOG_LEVEL_INF);

#define R_SHUNT_OHM     0.01f
#define SAMPLE_PERIOD_S 10

struct accumulator {
	float sum_a;
	uint32_t count;
	float latest_a;
	bool have_latest;
};

static struct accumulator acc[NUM_RELAYS];

static void sample_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(sample_work, sample_work_fn);

static void hourly_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(hourly_work, hourly_work_fn);

static enum sense_channel relay_sense_channel(uint8_t idx)
{
	return (idx == 0) ? SENSE_REL0 : SENSE_REL1;
}

static void sample_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	float v_bat = 0.0f;
	bool have_vbat = false;

	for (uint8_t i = 0; i < NUM_RELAYS; i++) {
		if (!relay_get(i)) {
			continue;
		}

		if (!have_vbat) {
			if (sense_read_v(SENSE_VBAT, &v_bat)) {
				LOG_WRN("VBAT read failed");
				break;
			}
			have_vbat = true;
		}

		float v_rel = 0.0f;
		if (sense_read_v(relay_sense_channel(i), &v_rel)) {
			LOG_WRN("rel%u sense read failed", i);
			continue;
		}

		float current = (v_bat - v_rel) / R_SHUNT_OHM;
		if (current < 0.0f) {
			current = 0.0f;
		}

		acc[i].sum_a += current;
		acc[i].count++;
		acc[i].latest_a = current;
		acc[i].have_latest = true;

		LOG_DBG("rel%u I = %d mA (vbat=%dmV vrel=%dmV)", i,
			(int)(current * 1000.0f),
			(int)(v_bat * 1000.0f),
			(int)(v_rel * 1000.0f));
	}

	k_work_reschedule(&sample_work, K_SECONDS(SAMPLE_PERIOD_S));
}

static void publish_one(uint8_t idx)
{
	if (!acc[idx].have_latest || acc[idx].count == 0) {
		return;
	}

	struct publish_event ev = {
		.type = PUB_RELAY_CURRENT,
		.relay = idx,
		.current_avg_a = acc[idx].sum_a / (float)acc[idx].count,
		.current_latest_a = acc[idx].latest_a,
	};
	zbus_chan_pub(&PUBLISH_CHAN, &ev, K_SECONDS(1));

	acc[idx].sum_a = 0.0f;
	acc[idx].count = 0;
	acc[idx].have_latest = false;
}

static void hourly_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	int64_t now_ms;
	int err = date_time_now(&now_ms);
	if (err) {
		k_work_reschedule(&hourly_work, K_SECONDS(60));
		return;
	}

	int64_t past_hour = (now_ms / 1000) % 3600;

	if (past_hour < 5) {
		LOG_INF("Hourly relay-current report");
		for (uint8_t i = 0; i < NUM_RELAYS; i++) {
			publish_one(i);
		}
	}

	k_work_reschedule(&hourly_work, K_SECONDS(3600 - past_hour));
}

void relay_current_init(void)
{
	k_work_reschedule(&sample_work, K_SECONDS(SAMPLE_PERIOD_S));
	k_work_reschedule(&hourly_work, K_SECONDS(60));
}
