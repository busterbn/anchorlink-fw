#ifndef CHARGING_H_
#define CHARGING_H_

#include <stdbool.h>
#include <stdint.h>

void charging_init(void);

/* Latest known charging state for battery idx (0 or 1). False if unknown. */
bool charging_get(uint8_t idx);

#endif /* CHARGING_H_ */
