/*
 * Copyright (c) 2025, Move Innovation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT ti_lp5817

#include <zephyr/kernel.h>
#include <zephyr/drivers/led.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>
#include <zephyr/init.h>

#include "lp5817.h"

LOG_MODULE_REGISTER(lp5817, CONFIG_LOG_DEFAULT_LEVEL);

// Forward declaration
static int lp5817_set_brightness(const struct device *dev, uint32_t led, uint8_t value);

static int lp5817_led_on(const struct device *dev, uint32_t led)
{
	// A brightness of 100% is considered "on"
	return lp5817_set_brightness(dev, led, 100);
}

static int lp5817_led_off(const struct device *dev, uint32_t led)
{
	return lp5817_set_brightness(dev, led, 0);
}

static int lp5817_set_brightness(const struct device *dev, uint32_t led, uint8_t value)
{
	const struct lp5817_config *config = dev->config;
	uint8_t pwm_reg, dc_reg;
	uint8_t pwm_val;

	if (led >= LP5817_NUM_LEDS) {
		LOG_ERR("Invalid LED index %d", led);
		return -EINVAL;
	}

	if (value > 100) {
		value = 100;
	}

	// Map percentage to 8-bit PWM value
	pwm_val = (value * 255) / 100;

	// Select the correct register based on the LED index
	switch (led) {
	case 0:
		pwm_reg = LP5817_REG_OUT0_MANUAL_PWM;
		dc_reg = LP5817_REG_OUT0_DC;
		break;
	case 1:
		pwm_reg = LP5817_REG_OUT1_MANUAL_PWM;
		dc_reg = LP5817_REG_OUT1_DC;
		break;
	case 2:
		pwm_reg = LP5817_REG_OUT2_MANUAL_PWM;
		dc_reg = LP5817_REG_OUT2_DC;
		break;
	default:
		return -EINVAL;
	}

	// If brightness is > 0, set dot current to max to enable output.
	// If brightness is 0, set dot current to 0.
	uint8_t dc_val = (value > 0) ? 0x32 : 0x00;
	if (i2c_reg_write_byte_dt(&config->i2c, dc_reg, dc_val)) {
		LOG_ERR("Failed to write DC for LED %d", led);
		return -EIO;
	}
	
	// Write the PWM value to set brightness
	if (i2c_reg_write_byte_dt(&config->i2c, pwm_reg, pwm_val)) {
		LOG_ERR("Failed to set brightness for LED %d", led);
		return -EIO;
	}

	return 0;
}

static int lp5817_init(const struct device *dev)
{
	const struct lp5817_config *config = dev->config;
	int ret;

	if (!device_is_ready(config->i2c.bus)) {
		LOG_ERR("I2C bus %s not ready", config->i2c.bus->name);
		return -ENODEV;
	}

	// 1. Enable the device
	ret = i2c_reg_write_byte_dt(&config->i2c, LP5817_REG_CHIP_EN, LP5817_CHIP_EN_BIT);
	if (ret < 0) {
		LOG_ERR("Failed to enable device");
		return -EIO;
	}
    
    // Wait for device to be ready (as per datasheet Fig 8-2)
    k_msleep(1);

	// 2. Set max current
	uint8_t max_current_bit = (config->max_current == 51) ? LP5817_DEV_CONFIG0_MAX_CURRENT : 0;
	ret = i2c_reg_write_byte_dt(&config->i2c, LP5817_REG_DEV_CONFIG0, max_current_bit);
	if (ret < 0) {
		LOG_ERR("Failed to set max current");
		return -EIO;
	}

	// 3. Enable all three output channels
	ret = i2c_reg_write_byte_dt(&config->i2c, LP5817_REG_DEV_CONFIG1, LP5817_DEV_CONFIG1_ALL_EN);
	if (ret < 0) {
		LOG_ERR("Failed to enable outputs");
		return -EIO;
	}

	// 4. Send UPDATE command to apply DEV_CONFIG changes
	ret = i2c_reg_write_byte_dt(&config->i2c, LP5817_REG_UPDATE_CMD, LP5817_UPDATE_CMD_VAL);
	if (ret < 0) {
		LOG_ERR("Failed to send UPDATE command");
		return -EIO;
	}

	// Initialize all LEDs to OFF state
	for (int i = 0; i < LP5817_NUM_LEDS; i++) {
		lp5817_led_off(dev, i);
	}

	return 0;
}

static const struct led_driver_api lp5817_led_api = {
	.on = lp5817_led_on,
	.off = lp5817_led_off,
	.set_brightness = lp5817_set_brightness,
};

#define LP5817_INIT(inst)                                                      \
	static struct lp5817_data lp5817_data_##inst;                                  \
	static const struct lp5817_config lp5817_config_##inst = {                 \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                                     \
		.max_current = DT_INST_PROP_OR(inst, max_current_ma, 25),              \
	};                                                                             \
	DEVICE_DT_INST_DEFINE(inst, &lp5817_init, NULL, &lp5817_data_##inst,        \
			      &lp5817_config_##inst, POST_KERNEL,                      \
			      99, &lp5817_led_api);

DT_INST_FOREACH_STATUS_OKAY(LP5817_INIT)
