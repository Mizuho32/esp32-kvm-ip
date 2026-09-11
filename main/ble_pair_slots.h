#pragma once

// Multi-device BLE pairing "slots" - like the Fn+1/2/3 device-switching
// button found on many commercial Bluetooth keyboards/mice. See
// mds/usb_hid/2026-09-12_ble_multi_pair.md for the design and why this
// replaced the old accept-then-reject-the-same-peer holdoff
// (mds/usb_hid/2026-09-11_ble_reconnect_holdoff.md) entirely.
//
// Each slot remembers at most one bonded peer's BLE identity address
// (persisted in NVS, survives reboots/ble_toggle cycles). Exactly one
// slot is ever "current" at a time (or none - idle) - this device only
// ever advertises to/connects with one peer at once, same as before;
// slots are a *quick-switch* mechanism (one bond remembered per slot,
// switched explicitly), not simultaneous multi-point connections (which
// would need a much larger rearchitecture of the single-connection BLE
// HID output layer - esp_ble_hidd_dev_s's conn_id/connected fields, both
// upstream and in this project's nimble_hidd_fork.c, are singular).

#include <stdbool.h>
#include "esp_err.h"
#include "nimble/ble.h"

#define BLE_PAIR_SLOT_COUNT 3

// Call once, from ble_hid_device_start() right after the stack comes up -
// re-arms directed advertising toward whichever slot was last active
// (persisted), so a reboot/ble_toggle cycle picks back up with the same
// device without needing an explicit ble_pair_switch() again. Does
// nothing (stays idle) if no slot has ever been active.
void ble_pair_slots_resume_on_start(void);

// Switches to slot `slot` (1..BLE_PAIR_SLOT_COUNT): directed-advertises to
// its previously-bonded peer - only that peer's link layer will even see
// a connectable advertisement. Disconnects the current connection first
// if one exists (asynchronous - actually takes effect once that
// disconnect completes, see ble_pair_slots_on_disconnect()).
// ESP_ERR_NOT_FOUND if the slot has never been paired with anything yet -
// use ble_pair_new() for that. ESP_ERR_INVALID_STATE if the BLE stack
// itself isn't started (ble_toggle true first - mds/usb_hid/2026-09-11_ble_dynamic_enable.md).
esp_err_t ble_pair_switch(int slot);

// Opens slot `slot` for pairing with a brand new device: forgets whatever
// was bonded there before and starts general/undirected advertising, same
// as before slots existed - any device can connect. Disconnects the
// current connection first if one exists (see ble_pair_switch()'s same
// note). Whichever peer connects next gets recorded into this slot
// (ble_pair_slots_on_connect()). ESP_ERR_INVALID_STATE if the BLE stack
// isn't started.
esp_err_t ble_pair_new(int slot);

// -1 if idle (nothing currently active/advertised-to/connected), else the
// 1-based slot number currently in play - whether still advertising
// (directed or, mid-ble_pair_new(), undirected/pairing), or connected.
int ble_pair_current_slot(void);

// True if `slot` has a bonded peer recorded (ble_pair_switch() would
// succeed rather than returning ESP_ERR_NOT_FOUND).
bool ble_pair_slot_bonded(int slot);

// Erases every slot's stored peer address and goes idle - called by
// ble_hid_device_unpair()'s "wipe everything" alongside ble_store_clear()
// (NimBLE's own bond store), so slots don't keep pointing at bonds that
// no longer exist.
void ble_pair_slots_forget_all(void);

// esp_hid_gap.c's nimble_hid_gap_event() calls these directly (it already
// has full GAP event access - peer address on connect, disconnect reason)
// rather than routing through ble_hid_device.c's higher-level
// esp_hidd_event_data_t, which exposes neither (see that struct's own
// fields).
void ble_pair_slots_on_connect(const ble_addr_t *peer_addr);
void ble_pair_slots_on_disconnect(bool deliberate);
