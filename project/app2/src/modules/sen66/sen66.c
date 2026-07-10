/*
 * Sensirion SEN66 air-quality sensor (PM, RH/T, VOC, NOx, CO2).
 *
 * Zephyr has no SEN66 driver, so this talks to it with raw I2C. Continuous
 * measurement is started at boot; the `sen66 read` shell command reads the
 * latest values and prints them.
 *
 * Bus: i2c1, address 0x6B. Each 16-bit word on the wire is followed by a
 * CRC-8 byte (poly 0x31, init 0xFF).
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/shell/shell.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(sen66, LOG_LEVEL_INF);

#define SEN66_ADDR              0x6b
#define CMD_START_MEASURE       0x0021
#define CMD_STOP_MEASURE        0x0104
#define CMD_GET_DATA_READY      0x0202
#define CMD_READ_MEASURED       0x0300

static const struct device *const i2c = DEVICE_DT_GET(DT_NODELABEL(i2c2));

static uint8_t crc8(const uint8_t *data, size_t len)
{
	uint8_t crc = 0xFF;

	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (int b = 0; b < 8; b++) {
			crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
		}
	}
	return crc;
}

static int send_cmd(uint16_t cmd)
{
	uint8_t buf[2] = { cmd >> 8, cmd & 0xFF };

	return i2c_write(i2c, buf, sizeof(buf), SEN66_ADDR);
}

/* Issue `cmd`, wait `delay_ms`, then read `words` CRC-checked 16-bit words. */
static int read_words(uint16_t cmd, uint16_t *out, size_t words, uint32_t delay_ms)
{
	uint8_t raw[3 * 16];

	if (words * 3 > sizeof(raw)) {
		return -EINVAL;
	}

	int err = send_cmd(cmd);
	if (err) {
		return err;
	}
	k_sleep(K_MSEC(delay_ms));

	err = i2c_read(i2c, raw, words * 3, SEN66_ADDR);
	if (err) {
		return err;
	}

	for (size_t i = 0; i < words; i++) {
		if (crc8(&raw[i * 3], 2) != raw[i * 3 + 2]) {
			return -EIO;
		}
		out[i] = (raw[i * 3] << 8) | raw[i * 3 + 1];
	}
	return 0;
}

static int sen66_init(void)
{
	if (!device_is_ready(i2c)) {
		LOG_WRN("i2c not ready, SEN66 disabled");
		return 0;
	}
	if (send_cmd(CMD_START_MEASURE)) {
		LOG_WRN("SEN66 not responding (start measurement failed)");
	}
	return 0;
}

SYS_INIT(sen66_init, APPLICATION, 90);

/* Print a fixed-point value: raw/divisor with two decimals, sign-aware. */
static void print_fixed(const struct shell *sh, const char *label, int32_t raw,
			int32_t divisor, const char *unit)
{
	int32_t whole = raw / divisor;
	int32_t rem = raw % divisor;
	if (rem < 0) {
		rem = -rem;
	}
	int32_t frac = rem * 100 / divisor;
	const char *sign = (raw < 0 && whole == 0) ? "-" : "";

	shell_print(sh, "  %-14s %s%d.%02d %s", label, sign, (int)whole, (int)frac, unit);
}

static int cmd_sen66_read(const struct shell *sh, size_t argc, char **argv)
{
	if (!device_is_ready(i2c)) {
		shell_error(sh, "i2c bus not ready");
		return -ENODEV;
	}

	/* Wait briefly for a fresh measurement (data-ready LSB != 0). */
	uint16_t ready = 0;
	for (int i = 0; i < 20; i++) {
		if (read_words(CMD_GET_DATA_READY, &ready, 1, 20) == 0 && (ready & 0xFF)) {
			break;
		}
		k_sleep(K_MSEC(100));
	}
	if (!(ready & 0xFF)) {
		shell_warn(sh, "No data ready yet (sensor warming up?), reading anyway");
	}

	uint16_t v[9];
	int err = read_words(CMD_READ_MEASURED, v, ARRAY_SIZE(v), 20);
	if (err) {
		shell_error(sh, "SEN66 read failed: %d", err);
		return err;
	}

	shell_print(sh, "SEN66 air quality:");
	print_fixed(sh, "PM1.0",       (uint16_t)v[0], 10,  "ug/m3");
	print_fixed(sh, "PM2.5",       (uint16_t)v[1], 10,  "ug/m3");
	print_fixed(sh, "PM4.0",       (uint16_t)v[2], 10,  "ug/m3");
	print_fixed(sh, "PM10",        (uint16_t)v[3], 10,  "ug/m3");
	print_fixed(sh, "Humidity",    (int16_t)v[4], 100, "%RH");
	print_fixed(sh, "Temperature", (int16_t)v[5], 200, "C");
	print_fixed(sh, "VOC index",   (int16_t)v[6], 10,  "");
	print_fixed(sh, "NOx index",   (int16_t)v[7], 10,  "");
	shell_print(sh, "  %-14s %u ppm", "CO2", v[8]);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sen66_cmds,
	SHELL_CMD(read, NULL, "Read all air-quality values", cmd_sen66_read),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(sen66, &sen66_cmds, "SEN66 air-quality sensor", NULL);
