#
# Copyright (c) 2023 Nordic Semiconductor
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

menu "Trigger"

config MQTT_SAMPLE_TRIGGER_THREAD_STACK_SIZE
	int "Thread stack size"
	default 1024

config MQTT_SAMPLE_TRIGGER_STREAM_INTERVAL_SECONDS
	int "Streaming mode publish interval"
	default 30

config MQTT_SAMPLE_TRIGGER_STREAM_TIMEOUT_SECONDS
	int "Streaming mode auto-stop timeout"
	default 300

module = MQTT_SAMPLE_TRIGGER
module-str = Trigger
source "subsys/logging/Kconfig.template.log_config"

endmenu # Trigger
