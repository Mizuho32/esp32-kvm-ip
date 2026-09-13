// Periodic internal-RAM heap logging - diagnostic only, no behavior
// effect on anything else. See mds/usb_hid/2026-09-13_ble_idle_crash.md:
// added while chasing a real-hardware crash (assert failed: ble_hs_init
// ble_hs.c:995, from ble_gattc_init() returning BLE_HS_ENOMEM) that hit
// ble_hid_device_start() after the board sat idle for hours. That doc's
// event-triggered start()/stop() heap logs only bracket a BLE toggle -
// they can't show *when* during a long idle stretch (BLE on or off)
// free memory actually moved, or whether it's a steady leak (block count
// climbing) vs. one-off fragmentation. This fills that gap: an
// always-running timer, independent of BLE state entirely, so its log
// lines interleave with whatever else is happening (ble true/false,
// pairing, WiFi events, ...) and can be correlated against them after
// the fact.
#ifndef _HEAP_MONITOR_H_
#define _HEAP_MONITOR_H_

#ifdef __cplusplus
extern "C" {
#endif

// Logs one baseline line immediately, then again every
// HEAP_MONITOR_INTERVAL_US (heap_monitor.c) forever. Call once, early in
// app_main() - before anything else has a chance to allocate, so the very
// first line is as close to "nothing has run yet" as this project can get.
void heap_monitor_init(void);

#ifdef __cplusplus
}
#endif

#endif /* _HEAP_MONITOR_H_ */
