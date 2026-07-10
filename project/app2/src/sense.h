#ifndef _SENSE_H_
#define _SENSE_H_

#include <stdint.h>
#include <stdbool.h>

enum sense_channel {
	SENSE_REL0 = 0,
	SENSE_REL1,
	SENSE_VBAT,
	SENSE_VBAT2,
	SENSE_IGNITION,
	SENSE_COUNT,
};

int sense_init(void);

/* Read a sense channel. Asserts the corresponding EN pin (if separate),
 * waits for settling, samples ADC, and returns millivolts at the pin. */
int sense_read_mv(enum sense_channel ch, int32_t *millivolts);

/* Convenience: returns voltage in volts. */
int sense_read_v(enum sense_channel ch, float *volts);

#endif /* _SENSE_H_ */
