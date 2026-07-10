#ifndef BATTERY_MONITOR_H_
#define BATTERY_MONITOR_H_

/* Read both batteries and publish a PUB_BAT_REPORT now. */
void battery_monitor_force_report(void);

#endif /* BATTERY_MONITOR_H_ */
