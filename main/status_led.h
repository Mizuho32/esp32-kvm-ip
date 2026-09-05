#ifndef STATUS_LED_H
#define STATUS_LED_H

#include <stdbool.h>

/**
 * Configures the onboard user LED (XIAO ESP32S3: GPIO21, active-low) as
 * an output and turns it off.
 */
void status_led_init(void);

/**
 * Turns the onboard user LED on or off - also stops any active blinking
 * (status_led_set_blinking()), so this always wins over it. Used by
 * power_manager.c (USB-suspend power saving) and wifi_manager.c (WiFi
 * connected/disconnected).
 */
void status_led_set(bool on);

/**
 * Starts/stops blinking the LED (see status_led.c for the interval) -
 * wifi_manager.c's own signal for "still trying to connect for the first
 * time since boot" (see its event_handler()). status_led_set() always
 * overrides this (stops the blink and forces a definite level), so
 * callers don't need to call status_led_set_blinking(false) themselves
 * before setting a definite state.
 */
void status_led_set_blinking(bool blinking);

#endif
