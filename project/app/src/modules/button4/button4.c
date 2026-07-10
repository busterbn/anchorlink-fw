/*
 * button4 - analog threshold input (former ignition line).
 *
 * The signal only reaches ~1 V when pressed, too low for a digital GPIO input,
 * so it is read via the SENSE_IGNITION ADC channel. The line is sampled every
 * 50 ms and compared against a threshold with hysteresis. A press (crossing
 * above the threshold) toggles relay 1 via CMD_CHAN, the same path used by the
 * on-board buttons and MQTT; each HIGH<->LOW transition is logged.
 *
 * nRF9151 has no analog comparator and the Zephyr ADC driver does not expose
 * the SAADC limit interrupt, so this polls rather than using a hardware IRQ.
 */

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/logging/log.h>

#include "message_channel.h"
#include "sense.h"

LOG_MODULE_REGISTER(button4, LOG_LEVEL_INF);

#define POLL_MS          50
#define THRESH_HI_MV     500   /* -> HIGH at/above this */
#define THRESH_LO_MV     350   /* -> LOW at/below this (hysteresis) */
#define TOGGLE_RELAY_IDX 0     /* relay 1 (the "relay1" topic) */

static void button4_thread(void)
{
	bool high = false;

	while (1) {
		k_sleep(K_MSEC(POLL_MS));

		int32_t mv;
		if (sense_read_mv(SENSE_IGNITION, &mv)) {
			continue;
		}

		if (!high && mv >= THRESH_HI_MV) {
			high = true;
			LOG_INF("button4 -> HIGH (%d mV)", mv);
			/* Pressed: toggle relay 1 through the shared command channel. */
			struct command cmd = { .action = CMD_TOGGLE_RELAY, .relay = TOGGLE_RELAY_IDX };
			zbus_chan_pub(&CMD_CHAN, &cmd, K_SECONDS(1));
		} else if (high && mv <= THRESH_LO_MV) {
			high = false;
			LOG_INF("button4 -> LOW (%d mV)", mv);
		}
	}
}

K_THREAD_DEFINE(button4_tid, 1024, button4_thread, NULL, NULL, NULL, 7, 0, 0);
