/*
 * Hardware watchdog supervisor.
 *
 * Feeds the SoC watchdog as long as the device is healthy. "Healthy" means
 * the MQTT broker is currently reachable, or has been within the last
 * DISCONNECT_REBOOT_MS. If the broker stays unreachable longer than that, the
 * supervisor stops feeding and the hardware watchdog resets the SoC, forcing a
 * fresh modem attach. The hardware watchdog also catches a hung supervisor
 * thread or kernel.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/drivers/watchdog.h>

#include "message_channel.h"

LOG_MODULE_REGISTER(watchdog, LOG_LEVEL_INF);

#define DISCONNECT_REBOOT_MS (30 * 60 * 1000)  /* reboot after 30 min offline */
#define WDT_TIMEOUT_MS       (60 * 1000)        /* hardware reset window */
#define FEED_INTERVAL_MS     (20 * 1000)        /* feed cadence while healthy */

static const struct device *const wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));

static atomic_t connected = ATOMIC_INIT(0);
static int64_t disconnected_since;

static void watchdog_cb(const struct zbus_channel *chan)
{
	const enum connection_status *status = zbus_chan_const_msg(chan);

	if (*status == CONNECTION_UP) {
		atomic_set(&connected, 1);
	} else {
		atomic_set(&connected, 0);
		disconnected_since = k_uptime_get();
	}
}

ZBUS_LISTENER_DEFINE(watchdog, watchdog_cb);

static bool healthy(void)
{
	if (atomic_get(&connected)) {
		return true;
	}
	return (k_uptime_get() - disconnected_since) < DISCONNECT_REBOOT_MS;
}

static void watchdog_thread(void)
{
	if (!device_is_ready(wdt)) {
		LOG_ERR("Watchdog device not ready, no reset protection");
		return;
	}

	struct wdt_timeout_cfg cfg = {
		.window.min = 0,
		.window.max = WDT_TIMEOUT_MS,
		.callback = NULL,
		.flags = WDT_FLAG_RESET_SOC,
	};

	int channel_id = wdt_install_timeout(wdt, &cfg);
	if (channel_id < 0) {
		LOG_ERR("wdt_install_timeout failed: %d", channel_id);
		return;
	}
	if (wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG)) {
		LOG_ERR("wdt_setup failed");
		return;
	}

	/* Grace period: 30 min from boot to establish the first connection. */
	disconnected_since = k_uptime_get();

	while (1) {
		if (healthy()) {
			wdt_feed(wdt, channel_id);
		} else {
			LOG_ERR("No MQTT connection for >%d min, allowing watchdog reset",
				DISCONNECT_REBOOT_MS / 60000);
			/* Stop feeding -> SoC resets within WDT_TIMEOUT_MS. */
		}
		k_sleep(K_MSEC(FEED_INTERVAL_MS));
	}
}

K_THREAD_DEFINE(watchdog_tid, 1024, watchdog_thread, NULL, NULL, NULL, 7, 0, 0);
