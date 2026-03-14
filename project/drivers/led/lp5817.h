/*
 * Copyright (c) 2025, Move Innovation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_LED_LP5817_H_
#define ZEPHYR_DRIVERS_LED_LP5817_H_

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/led.h>

/* Register Definitions */
#define LP5817_REG_CHIP_EN              0x00
#define LP5817_REG_DEV_CONFIG0          0x01
#define LP5817_REG_DEV_CONFIG1          0x02
#define LP5817_REG_DEV_CONFIG2          0x03
#define LP5817_REG_DEV_CONFIG3          0x04
#define LP5817_REG_SHUTDOWN_CMD         0x0D
#define LP5817_REG_RESET_CMD            0x0E
#define LP5817_REG_UPDATE_CMD           0x0F
#define LP5817_REG_FLAG_CLR             0x13
#define LP5817_REG_OUT0_DC              0x14
#define LP5817_REG_OUT1_DC              0x15
#define LP5817_REG_OUT2_DC              0x16
#define LP5817_REG_OUT0_MANUAL_PWM      0x18
#define LP5817_REG_OUT1_MANUAL_PWM      0x19
#define LP5817_REG_OUT2_MANUAL_PWM      0x1A
#define LP5817_REG_FLAG                 0x40

/* Register Bit Definitions */
#define LP5817_CHIP_EN_BIT              BIT(0)

#define LP5817_DEV_CONFIG0_MAX_CURRENT  BIT(0)

#define LP5817_DEV_CONFIG1_OUT0_EN      BIT(0)
#define LP5817_DEV_CONFIG1_OUT1_EN      BIT(1)
#define LP5817_DEV_CONFIG1_OUT2_EN      BIT(2)
#define LP5817_DEV_CONFIG1_ALL_EN       (LP5817_DEV_CONFIG1_OUT0_EN | \
                                         LP5817_DEV_CONFIG1_OUT1_EN | \
                                         LP5817_DEV_CONFIG1_OUT2_EN)

/* Command Values */
#define LP5817_RESET_CMD_VAL            0xCC
#define LP5817_UPDATE_CMD_VAL           0x55

/* Number of LED channels */
#define LP5817_NUM_LEDS                 3

/* Driver data structures */

struct lp5817_config {
	struct i2c_dt_spec i2c;
	uint8_t max_current;
};

struct lp5817_data {
	/* LED driver data */
};

#endif /* ZEPHYR_DRIVERS_LED_LP5817_H_ */
