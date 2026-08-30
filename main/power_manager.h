#ifndef POWER_MANAGER_H
#define POWER_MANAGER_H

#include <stdbool.h>

/**
 * Starts the background task that reacts to USB suspend/resume (see
 * power_manager_on_usb_suspend_changed() below). Call once at boot.
 * Compiled into both roles (Device role's main.c, Host role's
 * main_host.c) - harmless to call even if this board never gets a USB
 * Device suspend/resume at all (the task just blocks forever waiting for
 * the first notification).
 */
void power_manager_init(void);

/**
 * Reacts to USB Device suspend/resume (see usb_descriptors.c's
 * tud_suspend_cb()/tud_resume_cb(), which call this via a weak hook so
 * it links fine even in configs that never actually get a USB Device
 * connection - e.g. Host role without MAX3421E/RP2040-bridge type-c
 * output, see usb_device_typec.c).
 *
 * The actual WiFi-stop/light-sleep/LED reaction can be disabled per
 * board without a firmware rebuild via the Host-role-only mruby DSL
 * directive `usb_suspend_wifi_sleep false` (see mruby_filter.h) - e.g.
 * for a Host board whose WiFi also carries its own real-HID-forwarding
 * duty to a separate Device-role board, where stopping WiFi just because
 * the local type-c PC happens to be suspended would be harmful. Default
 * is enabled - see mds/usb_hid/2026-8-30_Sleep.md.
 *
 * Just a notification, not the reaction itself - only wakes up
 * power_manager_task (see power_manager.c) to (re-)check
 * usb_device_suspended(), since the actual WiFi stop / light-sleep-cycle /
 * WiFi restart sequence blocks for seconds at a time and must not run on
 * tinyusb's own task (the caller of tud_suspend_cb/tud_resume_cb) - doing
 * so would stop that task from ever processing the eventual resume event.
 *
 * Gated behind CONFIG_USB_SUSPEND_LIGHT_SLEEP_ENABLE (build-time kill
 * switch, distinct from the mruby toggle above - see
 * main/Kconfig.projbuild) for why real USB-resume-triggered wakeup isn't
 * possible on this chip/IDF combination (no such wake source exists for
 * ESP32-S3), so this is timer-cycled light sleep instead.
 */
void power_manager_on_usb_suspend_changed(bool suspended);

#endif
