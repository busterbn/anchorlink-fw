/*
 * Trigger module — manages idle/streaming modes.
 *
 * Idle mode (default): no periodic triggers, only on-demand via TRIGGER_CHAN.
 * Streaming mode: triggers TRIGGER_CHAN every 30s, auto-stops after 5 min.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>

#include "message_channel.h"

LOG_MODULE_REGISTER(trigger, CONFIG_MQTT_SAMPLE_TRIGGER_LOG_LEVEL);

ZBUS_SUBSCRIBER_DEFINE(trigger, 4);

static bool streaming;

/* Work items — timer callbacks run in ISR, so defer zbus publish to work queue */
static void send_trigger_work_fn(struct k_work *work);
static void stream_timeout_work_fn(struct k_work *work);

static K_WORK_DEFINE(send_trigger_work, send_trigger_work_fn);
static K_WORK_DEFINE(stream_timeout_work, stream_timeout_work_fn);

static void stream_timer_fn(struct k_timer *timer);
static void stream_timeout_fn(struct k_timer *timer);

static K_TIMER_DEFINE(stream_timer, stream_timer_fn, NULL);
static K_TIMER_DEFINE(stream_timeout, stream_timeout_fn, NULL);

static void send_trigger_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	int not_used = 0;
	int err = zbus_chan_pub(&TRIGGER_CHAN, &not_used, K_SECONDS(1));

	if (err) {
		LOG_ERR("zbus_chan_pub error: %d", err);
	}
}

static void stream_timer_fn(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	k_work_submit(&send_trigger_work);
}

static void stream_timeout_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	LOG_INF("Stream timeout — returning to idle mode");
	streaming = false;
	k_timer_stop(&stream_timer);
}

static void stream_timeout_fn(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	k_work_submit(&stream_timeout_work);
}

static void start_streaming(void)
{
	LOG_INF("Starting streaming mode");
	streaming = true;

	k_timer_start(&stream_timer,
		       K_SECONDS(CONFIG_MQTT_SAMPLE_TRIGGER_STREAM_INTERVAL_SECONDS),
		       K_SECONDS(CONFIG_MQTT_SAMPLE_TRIGGER_STREAM_INTERVAL_SECONDS));

	k_timer_start(&stream_timeout,
		       K_SECONDS(CONFIG_MQTT_SAMPLE_TRIGGER_STREAM_TIMEOUT_SECONDS),
		       K_NO_WAIT);

	/* Publish state immediately */
	k_work_submit(&send_trigger_work);
}

static void stop_streaming(void)
{
	LOG_INF("Stopping streaming mode");
	streaming = false;
	k_timer_stop(&stream_timer);
	k_timer_stop(&stream_timeout);
}

static void trigger_task(void)
{
	const struct zbus_channel *chan;

	while (!zbus_sub_wait(&trigger, &chan, K_FOREVER)) {
		if (&CMD_CHAN == chan) {
			struct command cmd;
			int err = zbus_chan_read(&CMD_CHAN, &cmd, K_SECONDS(1));

			if (err) {
				continue;
			}

			switch (cmd.action) {
			case CMD_START_STREAM:
				start_streaming();
				break;
			case CMD_STOP_STREAM:
				stop_streaming();
				break;
			default:
				break;
			}
		}
	}
}

K_THREAD_DEFINE(trigger_task_id,
		CONFIG_MQTT_SAMPLE_TRIGGER_THREAD_STACK_SIZE,
		trigger_task, NULL, NULL, NULL, 3, 0, 0);
