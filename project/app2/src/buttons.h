#ifndef _BUTTONS_H_
#define _BUTTONS_H_

#include <stdint.h>

#define NUM_BUTTONS 3

typedef void (*button_pressed_cb_t)(uint8_t idx);
typedef void (*button_long_pressed_cb_t)(uint8_t idx);

int buttons_init(button_pressed_cb_t cb, button_long_pressed_cb_t long_cb);

#endif /* _BUTTONS_H_ */
