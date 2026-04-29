#
# Copyright (c) 2023 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mqtt)

target_sources(app PRIVATE src/main.c src/relays.c src/sense.c src/buttons.c src/gps.c src/charging.c)
target_include_directories(app PRIVATE src)

# Include files that are common for all modules
add_subdirectory(src/common)

# Include mandatory module source folders
add_subdirectory(src/modules/network)
add_subdirectory(src/modules/transport)
add_subdirectory(src/modules/error)
add_subdirectory(src/modules/fota)

# Optional modules
add_subdirectory_ifdef(CONFIG_MQTT_SAMPLE_LED src/modules/led)
