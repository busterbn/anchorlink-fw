# Copyright (c) 2025, My Company
# SPDX-License-Identifier: Apache-2.0

zephyr_library()

if(CONFIG_LED_LP5817)
    zephyr_library_sources(lp5817.c)
endif()
