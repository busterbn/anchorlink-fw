#ifndef _POWER_H_
#define _POWER_H_

#include <stdint.h>

int power_init(void);
int power_read_battery_mv(int32_t *millivolts);
int power_read_current_ma(int32_t *milliamps);

#endif /* _POWER_H_ */
