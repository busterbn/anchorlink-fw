#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/shell/shell.h>
#include <stdlib.h>
#include <math.h>

#include <nrf_modem.h>
#include <nrf_modem_gnss.h>
#include <modem/lte_lc.h>

#include "message_channel.h"

LOG_MODULE_REGISTER(gps, LOG_LEVEL_INF);

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define EARTH_RADIUS_M             6371000.0
#define GPS_FIX_RETRY_SECONDS      600
#define ANCHOR_CHECK_INTERVAL_SEC  30

ZBUS_SUBSCRIBER_DEFINE(gps_sub, 4);

enum gps_mode {
	GPS_IDLE,
	GPS_FIX_REQUESTED,
	GPS_ANCHOR_ACQUIRING,
	GPS_ANCHOR_MONITORING,
};

static enum gps_mode mode = GPS_IDLE;
static double anchor_lat;
static double anchor_lon;
static uint32_t anchor_radius_m;
static bool gnss_configured;

/* Latest PVT snapshot for status / diagnostics. */
static uint8_t last_tracked;
static uint8_t last_in_fix;
static bool last_blocked;

/* Deferred handling of GNSS events (event callback runs in a small modem
 * thread context — keep it minimal). */
static double pending_fix_lat;
static double pending_fix_lon;
static atomic_t pending_fix_flag;
static atomic_t pending_timeout_flag;
static void gnss_evt_work_fn(struct k_work *work);
static K_WORK_DEFINE(gnss_evt_work, gnss_evt_work_fn);

static void anchor_recheck_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(anchor_recheck_work, anchor_recheck_work_fn);

static double distance_m(double lat1, double lon1, double lat2, double lon2)
{
	double dlat = (lat2 - lat1) * M_PI / 180.0;
	double dlon = (lon2 - lon1) * M_PI / 180.0;
	double a = sin(dlat / 2) * sin(dlat / 2) +
		   cos(lat1 * M_PI / 180.0) * cos(lat2 * M_PI / 180.0) *
			   sin(dlon / 2) * sin(dlon / 2);
	double c = 2.0 * asin(sqrt(a));
	return EARTH_RADIUS_M * c;
}

static void publish_gps(double lat, double lon)
{
	struct publish_event ev = {
		.type = PUB_GPS,
		.latitude = lat,
		.longitude = lon,
	};
	zbus_chan_pub(&PUBLISH_CHAN, &ev, K_SECONDS(1));
}

static void publish_anchor_alarm(double lat, double lon, uint32_t dist)
{
	struct publish_event ev = {
		.type = PUB_ANCHOR_ALARM,
		.latitude = lat,
		.longitude = lon,
		.distance_m = dist,
	};
	zbus_chan_pub(&PUBLISH_CHAN, &ev, K_SECONDS(1));
}

static int gnss_start_fix(void)
{
	if (nrf_modem_gnss_fix_interval_set(0) != 0) {
		return -1;
	}
	if (nrf_modem_gnss_fix_retry_set(GPS_FIX_RETRY_SECONDS) != 0) {
		return -1;
	}
	return nrf_modem_gnss_start();
}

static void anchor_recheck_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	if (mode != GPS_ANCHOR_MONITORING) {
		return;
	}

	LOG_INF("Anchor: re-checking GPS");
	if (gnss_start_fix() != 0) {
		LOG_ERR("Failed to start GNSS for anchor recheck");
		k_work_schedule(&anchor_recheck_work,
				K_SECONDS(ANCHOR_CHECK_INTERVAL_SEC));
	}
}

static void on_fix(double lat, double lon)
{
	nrf_modem_gnss_stop();

	switch (mode) {
	case GPS_FIX_REQUESTED:
		publish_gps(lat, lon);
		mode = GPS_IDLE;
		break;

	case GPS_ANCHOR_ACQUIRING:
		anchor_lat = lat;
		anchor_lon = lon;
		publish_gps(lat, lon);
		mode = GPS_ANCHOR_MONITORING;
		LOG_INF("Anchor set, radius %u m", anchor_radius_m);
		k_work_schedule(&anchor_recheck_work,
				K_SECONDS(ANCHOR_CHECK_INTERVAL_SEC));
		break;

	case GPS_ANCHOR_MONITORING: {
		double d = distance_m(lat, lon, anchor_lat, anchor_lon);
		LOG_INF("Anchor: distance %d m (limit %u)",
			(int)d, anchor_radius_m);
		if (d > (double)anchor_radius_m) {
			LOG_WRN("Anchor alarm triggered (%d m)", (int)d);
			publish_anchor_alarm(lat, lon, (uint32_t)d);
			mode = GPS_IDLE;
		} else {
			k_work_schedule(&anchor_recheck_work,
					K_SECONDS(ANCHOR_CHECK_INTERVAL_SEC));
		}
		break;
	}

	default:
		break;
	}
}

static void on_fix_timeout(void)
{
	LOG_WRN("GNSS fix timeout");
	nrf_modem_gnss_stop();

	if (mode == GPS_ANCHOR_MONITORING) {
		k_work_schedule(&anchor_recheck_work,
				K_SECONDS(ANCHOR_CHECK_INTERVAL_SEC));
	} else if (mode == GPS_ANCHOR_ACQUIRING) {
		LOG_WRN("Anchor acquire failed; clearing alarm");
		mode = GPS_IDLE;
	} else {
		mode = GPS_IDLE;
	}
}

static void update_pvt_stats(struct nrf_modem_gnss_pvt_data_frame *pvt)
{
	uint8_t tracked = 0;
	uint8_t in_fix = 0;

	for (int i = 0; i < NRF_MODEM_GNSS_MAX_SATELLITES; i++) {
		if (pvt->sv[i].sv > 0) {
			tracked++;
			if (pvt->sv[i].flags & NRF_MODEM_GNSS_SV_FLAG_USED_IN_FIX) {
				in_fix++;
			}
		}
	}
	last_tracked = tracked;
	last_in_fix = in_fix;
}

/* Runs in the system workqueue — safe to publish on zbus, stop GNSS, etc. */
static void gnss_evt_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	if (atomic_cas(&pending_fix_flag, 1, 0)) {
		on_fix(pending_fix_lat, pending_fix_lon);
	}
	if (atomic_cas(&pending_timeout_flag, 1, 0)) {
		on_fix_timeout();
	}
}

/* Event handler runs in the modem callback context (small stack). Keep it
 * minimal: only read PVT and defer heavy work to the system workqueue.
 */
static void gnss_event_handler(int event)
{
	struct nrf_modem_gnss_pvt_data_frame pvt;

	switch (event) {
	case NRF_MODEM_GNSS_EVT_PVT:
		if (nrf_modem_gnss_read(&pvt, sizeof(pvt),
					NRF_MODEM_GNSS_DATA_PVT) == 0) {
			update_pvt_stats(&pvt);
		}
		break;
	case NRF_MODEM_GNSS_EVT_FIX:
		if (nrf_modem_gnss_read(&pvt, sizeof(pvt),
					NRF_MODEM_GNSS_DATA_PVT) == 0) {
			update_pvt_stats(&pvt);
			pending_fix_lat = pvt.latitude;
			pending_fix_lon = pvt.longitude;
			atomic_set(&pending_fix_flag, 1);
			k_work_submit(&gnss_evt_work);
		}
		break;
	case NRF_MODEM_GNSS_EVT_SLEEP_AFTER_TIMEOUT:
		atomic_set(&pending_timeout_flag, 1);
		k_work_submit(&gnss_evt_work);
		break;
	case NRF_MODEM_GNSS_EVT_BLOCKED:
		last_blocked = true;
		break;
	case NRF_MODEM_GNSS_EVT_UNBLOCKED:
		last_blocked = false;
		break;
	default:
		break;
	}
}

static int gnss_configure(void)
{
	if (gnss_configured) {
		return 0;
	}
	/* Activate GNSS in the modem functional mode (LTE stays up alongside). */
	int err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_GNSS);
	if (err) {
		LOG_ERR("Failed to activate GNSS functional mode: %d", err);
		return -1;
	}
	if (nrf_modem_gnss_event_handler_set(gnss_event_handler) != 0) {
		LOG_ERR("Failed to set GNSS event handler");
		return -1;
	}
	if (nrf_modem_gnss_use_case_set(
		    NRF_MODEM_GNSS_USE_CASE_MULTIPLE_HOT_START) != 0) {
		LOG_WRN("Failed to set GNSS use case");
	}
	gnss_configured = true;
	LOG_INF("GNSS configured");
	return 0;
}

static void anchor_clear(void)
{
	k_work_cancel_delayable(&anchor_recheck_work);
	if (mode == GPS_ANCHOR_ACQUIRING || mode == GPS_ANCHOR_MONITORING) {
		nrf_modem_gnss_stop();
	}
	mode = GPS_IDLE;
}

static void handle_gps_request(void)
{
	if (gnss_configure() != 0) {
		return;
	}
	if (mode != GPS_IDLE) {
		LOG_WRN("GPS busy (mode=%d), ignoring request", mode);
		return;
	}
	mode = GPS_FIX_REQUESTED;
	if (gnss_start_fix() != 0) {
		LOG_ERR("Failed to start GNSS");
		mode = GPS_IDLE;
	}
}

static void handle_anchor_alarm(uint32_t radius_m)
{
	if (radius_m == 0) {
		LOG_INF("Anchor alarm cleared");
		anchor_clear();
		return;
	}
	if (gnss_configure() != 0) {
		return;
	}
	anchor_clear();
	anchor_radius_m = radius_m;
	mode = GPS_ANCHOR_ACQUIRING;
	if (gnss_start_fix() != 0) {
		LOG_ERR("Failed to start GNSS for anchor");
		mode = GPS_IDLE;
	}
}

static void gps_thread(void)
{
	const struct zbus_channel *chan;

	while (!nrf_modem_is_initialized()) {
		k_msleep(200);
	}

	while (!zbus_sub_wait(&gps_sub, &chan, K_FOREVER)) {
		if (chan != &CMD_CHAN) {
			continue;
		}
		struct command cmd;
		if (zbus_chan_read(&CMD_CHAN, &cmd, K_SECONDS(1))) {
			continue;
		}
		switch (cmd.action) {
		case CMD_REQUEST_GPS:
			handle_gps_request();
			break;
		case CMD_SET_ANCHOR_ALARM:
			handle_anchor_alarm(cmd.distance_m);
			break;
		default:
			break;
		}
	}
}

K_THREAD_DEFINE(gps_thread_id, 2048, gps_thread, NULL, NULL, NULL, 5, 0, 0);

/* ------------------------------------------------------------------ */
/* Shell commands — same actions the MQTT cmd topic dispatches.       */
/* ------------------------------------------------------------------ */

static void shell_dispatch(struct command *cmd)
{
	int err = zbus_chan_pub(&CMD_CHAN, cmd, K_SECONDS(1));
	if (err) {
		LOG_ERR("zbus_chan_pub failed: %d", err);
	}
}

static int cmd_gps_fix(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct command cmd = { .action = CMD_REQUEST_GPS };
	shell_dispatch(&cmd);
	shell_print(sh, "GPS fix requested");
	return 0;
}

static int cmd_gps_anchor(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 2) {
		shell_error(sh, "usage: gps anchor <meters>  (0 to cancel)");
		return -EINVAL;
	}
	long radius = strtol(argv[1], NULL, 10);
	if (radius < 0) {
		shell_error(sh, "radius must be >= 0");
		return -EINVAL;
	}

	struct command cmd = {
		.action = CMD_SET_ANCHOR_ALARM,
		.distance_m = (uint32_t)radius,
	};
	shell_dispatch(&cmd);
	if (radius == 0) {
		shell_print(sh, "Anchor alarm cleared");
	} else {
		shell_print(sh, "Anchor alarm set, radius %ld m", radius);
	}
	return 0;
}

static int cmd_gps_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	const char *mode_str;
	switch (mode) {
	case GPS_IDLE:               mode_str = "idle"; break;
	case GPS_FIX_REQUESTED:      mode_str = "fix-requested"; break;
	case GPS_ANCHOR_ACQUIRING:   mode_str = "anchor-acquiring"; break;
	case GPS_ANCHOR_MONITORING:  mode_str = "anchor-monitoring"; break;
	default:                     mode_str = "?"; break;
	}
	shell_print(sh, "mode:           %s", mode_str);
	shell_print(sh, "tracked sats:   %u", last_tracked);
	shell_print(sh, "sats used:      %u", last_in_fix);
	shell_print(sh, "blocked by LTE: %s", last_blocked ? "yes" : "no");
	if (mode == GPS_ANCHOR_MONITORING) {
		shell_print(sh, "anchor radius:  %u m", anchor_radius_m);
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(gps_cmds,
	SHELL_CMD(fix,    NULL, "Request a single GPS fix",            cmd_gps_fix),
	SHELL_CMD(anchor, NULL, "Set/clear anchor alarm: anchor <m>",  cmd_gps_anchor),
	SHELL_CMD(status, NULL, "Show current GPS module state",       cmd_gps_status),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(gps, &gps_cmds, "GPS commands", NULL);
