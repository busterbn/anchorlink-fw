#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>

#include <memfault/nrfconnect_port/fota.h>

#include "message_channel.h"

LOG_MODULE_REGISTER(fota, LOG_LEVEL_INF);

ZBUS_SUBSCRIBER_DEFINE(fota, 4);

#define OTA_CHECK_INTERVAL K_HOURS(24)
#define POST_CONNECT_SETTLE K_SECONDS(10)

static void wait_for_network_up(void)
{
	const struct zbus_channel *chan;
	enum network_status status;

	while (!zbus_sub_wait(&fota, &chan, K_FOREVER)) {
		if (chan != &NETWORK_CHAN) {
			continue;
		}
		if (zbus_chan_read(&NETWORK_CHAN, &status, K_SECONDS(1))) {
			continue;
		}
		if (status == NETWORK_CONNECTED) {
			return;
		}
	}
}

static void fota_thread(void)
{
	LOG_INF("Waiting for network");
	wait_for_network_up();
	k_sleep(POST_CONNECT_SETTLE);

	while (1) {
		LOG_INF("Checking for OTA update");
		int rv = memfault_fota_start();

		if (rv == 1) {
			LOG_INF("Update available, downloading. Device will reboot.");
		} else if (rv == 0) {
			LOG_INF("No update available");
		} else {
			LOG_WRN("OTA check failed: %d", rv);
		}

		k_sleep(OTA_CHECK_INTERVAL);
	}
}

K_THREAD_DEFINE(fota_tid, 2048, fota_thread, NULL, NULL, NULL, 7, 0, 0);
