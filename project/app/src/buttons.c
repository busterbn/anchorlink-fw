#include <zephyr/kernel.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>

#include "buttons.h"

LOG_MODULE_REGISTER(buttons, LOG_LEVEL_INF);

#define LONG_PRESS_MS 3000

static button_pressed_cb_t pressed_cb;
static button_long_pressed_cb_t long_pressed_cb;

static uint8_t long_press_idx;

static void long_press_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	if (long_pressed_cb) {
		long_pressed_cb(long_press_idx);
	}
}

static K_WORK_DELAYABLE_DEFINE(long_press_work, long_press_handler);

static int code_to_idx(uint16_t code)
{
	switch (code) {
	case INPUT_KEY_0: return 0;
	case INPUT_KEY_1: return 1;
	case INPUT_KEY_2: return 2;
	default: return -1;
	}
}

static void input_event_cb(struct input_event *evt, void *user_data)
{
	if (evt->type != INPUT_EV_KEY) {
		return;
	}

	int idx = code_to_idx(evt->code);
	if (idx < 0) {
		return;
	}

	if (evt->value) {
		if (idx == 0) {
			long_press_idx = idx;
			k_work_reschedule(&long_press_work, K_MSEC(LONG_PRESS_MS));
		}
		if (pressed_cb) {
			pressed_cb(idx);
		}
	} else {
		if (idx == 0) {
			k_work_cancel_delayable(&long_press_work);
		}
	}
}

INPUT_CALLBACK_DEFINE(NULL, input_event_cb, NULL);

int buttons_init(button_pressed_cb_t cb, button_long_pressed_cb_t long_cb)
{
	pressed_cb = cb;
	long_pressed_cb = long_cb;
	LOG_INF("Buttons initialized");
	return 0;
}
