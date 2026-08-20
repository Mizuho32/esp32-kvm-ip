#ifndef STATUS_LED_H
#define STATUS_LED_H

#include <stdbool.h>

/**
 * Configures the onboard user LED (XIAO ESP32S3: GPIO21, active-low) as
 * an output and turns it off.
 */
void status_led_init(void);

/**
 * Turns the onboard user LED on or off.
 */
void status_led_set(bool on);

#endif
