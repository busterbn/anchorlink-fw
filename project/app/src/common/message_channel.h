/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _MESSAGE_CHANNEL_H_
#define _MESSAGE_CHANNEL_H_

#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log_ctrl.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SEND_FATAL_ERROR()									\
	int not_used = -1;									\
	if (zbus_chan_pub(&FATAL_ERROR_CHAN, &not_used, K_SECONDS(10))) {			\
		LOG_ERR("Sending a message on the fatal error channel failed, rebooting");	\
		LOG_PANIC();									\
		IF_ENABLED(CONFIG_REBOOT, (sys_reboot(0)));					\
	}

enum network_status {
	NETWORK_DISCONNECTED,
	NETWORK_CONNECTED,
};

enum cmd_action {
	CMD_TOGGLE_RELAY,   /* relay = 0 or 1 */
	CMD_REPORT_BAT,
};

struct command {
	enum cmd_action action;
	uint8_t relay;
};

enum publish_event_type {
	PUB_RELAY_STATE,
	PUB_BAT_REPORT,
};

struct publish_event {
	enum publish_event_type type;
	/* PUB_RELAY_STATE */
	uint8_t relay;
	bool state;
	/* PUB_BAT_REPORT */
	float bat1_v;
	float bat2_v;
};

ZBUS_CHAN_DECLARE(NETWORK_CHAN, FATAL_ERROR_CHAN, CMD_CHAN, PUBLISH_CHAN);

#ifdef __cplusplus
}
#endif

#endif /* _MESSAGE_CHANNEL_H_ */
