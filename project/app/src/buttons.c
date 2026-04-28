#include <zephyr/kernel.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>

#include "buttons.h"

LOG_MODULE_REGISTER(buttons, LOG_LEVEL_INF);

static button_pressed_cb_t pressed_cb;

static void input_event_cb(struct input_event *evt, void *user_data)
{
	if (evt->type != INPUT_EV_KEY || evt->value == 0) {
		return;
	}

	uint8_t idx;
	switch (evt->code) {
	case INPUT_KEY_0: idx = 0; break;
	case INPUT_KEY_1: idx = 1; break;
	case INPUT_KEY_2: idx = 2; break;
	default: return;
	}

	if (pressed_cb) {
		pressed_cb(idx);
	}
}

INPUT_CALLBACK_DEFINE(NULL, input_event_cb, NULL);

int buttons_init(button_pressed_cb_t cb)
{
	pressed_cb = cb;
	LOG_INF("Buttons initialized");
	return 0;
}
