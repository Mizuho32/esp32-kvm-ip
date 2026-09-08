#ifndef STATUS_LED_H
#define STATUS_LED_H

#include <stdbool.h>

/**
 * Configures the onboard user LED (XIAO ESP32S3: GPIO21, active-low) as
 * an output and turns it off.
 */
void status_led_init(void);

/**
 * Turns the onboard user LED on or off - a definite level, always shown
 * immediately. Also cancels the WiFi "connecting" blink
 * (status_led_set_blinking()) if it was running, same as before (callers
 * don't need to call status_led_set_blinking(false) themselves first) -
 * used by power_manager.c (USB-suspend power saving) and wifi_manager.c
 * (WiFi connected/disconnected).
 *
 * Does *not* cancel status_led_set_ble_advertising()'s own desire - that
 * flag is owned exclusively by ble_hid_device.c - but its visibility is
 * gated on `on`: a `false` here (WiFi off/disconnected/USB-suspended)
 * always shows solid off regardless, matching
 * mds/usb_hid/2026-09-07_ble_hid_sink_plan.md's "WiFi優先" rule (BLE's own
 * status only ever appears once WiFi is stable and lit). A later `true`
 * call resumes the BLE pattern automatically if it's still wanted - no
 * extra call from ble_hid_device.c needed.
 */
void status_led_set(bool on);

/**
 * Starts/stops blinking the LED in the WiFi "still trying to connect for
 * the first time since boot" pattern (see status_led.c for the interval)
 * - wifi_manager.c's own signal (see its event_handler()). Highest
 * priority of the two patterns below: while active, it's shown regardless
 * of status_led_set_ble_advertising()'s state (see
 * mds/usb_hid/2026-09-07_ble_hid_sink_plan.md's "WiFi優先" rule -
 * there's only one LED).
 */
void status_led_set_blinking(bool blinking);

/**
 * Starts/stops a second, visually distinct blink pattern (short
 * double-blink + pause, vs. the WiFi pattern's steady metronome toggle -
 * see status_led.c) used by ble_hid_device.c to show "advertising,
 * waiting for a BLE central to connect/reconnect". Only actually visible
 * while status_led_set_blinking() isn't active *and* the last
 * status_led_set() call was `true` (WiFi connected, not suspended) - see
 * status_led_set()'s doc comment and
 * mds/usb_hid/2026-09-07_ble_hid_sink_plan.md's "WiFi優先" rule. Call with
 * `false` once a BLE central actually connects (falls back to solid on)
 * or advertising stops for any other reason.
 */
void status_led_set_ble_advertising(bool advertising);

#endif
