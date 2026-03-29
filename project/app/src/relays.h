#ifndef _RELAYS_H_
#define _RELAYS_H_

#include <zephyr/drivers/gpio.h>

#define NUM_RELAYS 5

int relays_init(void);
int relay_set(uint8_t idx, bool on);
bool relay_get(uint8_t idx);
int relays_set_all(uint8_t mask);

#endif /* _RELAYS_H_ */
