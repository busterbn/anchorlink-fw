/*
 * Sampler module — reads sensors and publishes structured payload.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <date_time.h>

#include "message_channel.h"
#include "relays.h"
#include "power.h"

LOG_MODULE_REGISTER(sampler, CONFIG_MQTT_SAMPLE_SAMPLER_LOG_LEVEL);

ZBUS_SUBSCRIBER_DEFINE(sampler, CONFIG_MQTT_SAMPLE_SAMPLER_MESSAGE_QUEUE_SIZE);

static void sample(void)
{
	struct payload payload = { 0 };
	int32_t battery_mv = 0;
	int32_t current_ma = 0;

	power_read_battery_mv(&battery_mv);
	power_read_current_ma(&current_ma);

	payload.voltage = battery_mv / 1000.0f;
	payload.power_w = (battery_mv / 1000.0f) * (current_ma / 1000.0f);

	for (int i = 0; i < NUM_RELAYS; i++) {
		payload.relays[i] = relay_get(i);
	}

	int64_t ts;
	if (date_time_now(&ts) == 0) {
		payload.ts = ts / 1000; /* ms to seconds */
	}

	int err = zbus_chan_pub(&PAYLOAD_CHAN, &payload, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void sampler_task(void)
{
	const struct zbus_channel *chan;

	while (!zbus_sub_wait(&sampler, &chan, K_FOREVER)) {
		if (&TRIGGER_CHAN == chan) {
			sample();
		}
	}
}

K_THREAD_DEFINE(sampler_task_id,
		CONFIG_MQTT_SAMPLE_SAMPLER_THREAD_STACK_SIZE,
		sampler_task, NULL, NULL, NULL, 3, 0, 0);
