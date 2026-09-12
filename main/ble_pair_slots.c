#include "ble_pair_slots.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#include "host/ble_gap.h"
#include "host/ble_store.h"

#include "ble_hid_device.h"
#include "esp_hid_gap.h"

#define TAG "BLE_PAIR"

// NVS-backed slot storage - mirrors debug_stream.c's NVS persistence
// pattern (NVS_NAMESPACE there). Namespace/key names are short: NVS caps
// both at 15 bytes. Key "active": which slot to resume_on_start() into
// (u8, 0 = none). Keys "slot1".."slot3": each slot's bonded peer identity
// address, stored as a 7-byte blob (1 byte ble_addr_t.type + 6 bytes
// .val) - presence of the key *is* "this slot is bonded", no separate
// flag needed (nvs_get_blob() returns ESP_ERR_NVS_NOT_FOUND for a slot
// that's never been written, or was erased by ble_pair_new()/forget_all()).
#define NVS_NAMESPACE "blepair"
#define NVS_KEY_ACTIVE "active"

// Slot 1 advertises under this device's real (public) address, same as
// before slots existed - untouched, so nothing already bonded to it
// notices any difference. Slots 2+ each get their own fixed random
// static address instead (real-hardware finding: switching slots while
// keeping one shared address let a host still bonded to a *different*
// slot recognize this device's directed advertisement as "the same
// device" even though it couldn't connect to it, and try to reconnect
// anyway, interfering with whichever slot the ad was actually meant for
// - see mds/usb_hid/2026-09-12_ble_multi_pair.md's follow-up). Giving
// each extra slot its own identity makes it a genuinely different,
// unrecognized device to everyone but its own bonded peer.
//
// Fixed/hardcoded (not generated at runtime) so they're stable across
// reboots without needing their own NVS storage - the bond a peer forms
// with slot N's address only stays valid if that address never changes.
// Static random address rules (host/ble_hs_id.h): host byte order
// (val[0] = LSB), top two bits of the most significant byte (val[5])
// must be '1 1' for a *static* (non-resolvable-private) address - picked
// 0xC0 (0b11000000) here, safely satisfying that with the rest zeroed
// out except a slot-number tiebreaker in val[0].
static const ble_addr_t s_slot_own_addr[BLE_PAIR_SLOT_COUNT + 1] = {
    [2] = { .type = BLE_ADDR_RANDOM, .val = { 0x02, 0x00, 0x00, 0x00, 0x00, 0xC0 } },
    [3] = { .type = BLE_ADDR_RANDOM, .val = { 0x03, 0x00, 0x00, 0x00, 0x00, 0xC0 } },
};

// NULL for slot 1 (this device's real address) or an out-of-range slot;
// &s_slot_own_addr[slot] for slot 2/3.
static const ble_addr_t *own_addr_for_slot(int slot)
{
    if (slot < 2 || slot > BLE_PAIR_SLOT_COUNT) {
        return NULL;
    }
    return &s_slot_own_addr[slot];
}

static void slot_key(int slot, char *buf, size_t buf_size)
{
    snprintf(buf, buf_size, "slot%d", slot);
}

static bool load_slot_addr(int slot, ble_addr_t *out)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    char key[8];
    slot_key(slot, key, sizeof key);
    size_t len = sizeof(*out);
    esp_err_t err = nvs_get_blob(h, key, out, &len);
    nvs_close(h);
    return err == ESP_OK && len == sizeof(*out);
}

static void save_slot_addr(int slot, const ble_addr_t *addr)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open() failed - slot %d's new bond won't survive a reboot this time", slot);
        return;
    }
    char key[8];
    slot_key(slot, key, sizeof key);
    nvs_set_blob(h, key, addr, sizeof(*addr));
    nvs_commit(h);
    nvs_close(h);
}

static void erase_slot_addr(int slot)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    char key[8];
    slot_key(slot, key, sizeof key);
    nvs_erase_key(h, key); // ESP_ERR_NVS_NOT_FOUND if already empty - fine, nothing to do
    nvs_commit(h);
    nvs_close(h);
}

static void save_active_slot(int slot)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, NVS_KEY_ACTIVE, (uint8_t)slot);
    nvs_commit(h);
    nvs_close(h);
}

static int load_active_slot(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return -1;
    }
    uint8_t v = 0;
    esp_err_t err = nvs_get_u8(h, NVS_KEY_ACTIVE, &v);
    nvs_close(h);
    if (err != ESP_OK || v < 1 || v > BLE_PAIR_SLOT_COUNT) {
        return -1;
    }
    return v;
}

// -1 = idle (nothing advertised-to/connected right now). s_pairing_mode
// only means something when s_current_slot >= 0: true while that slot is
// open for a *new* bond (undirected advertising, see ble_pair_new()) and
// hasn't recorded a peer yet; false once bonded (or for a
// ble_pair_switch() to an already-bonded slot, which is never "pairing").
static int s_current_slot = -1;
static bool s_pairing_mode = false;

// Set by ble_pair_switch()/ble_pair_new() when called while still
// connected to a *different* slot - the actual switch can't happen until
// that disconnect completes (ble_gap_terminate() is asynchronous), so
// ble_pair_slots_on_disconnect() checks this first and, if set, honors it
// instead of its normal deliberate/involuntary reasoning.
static int s_pending_slot = -1;
static bool s_pending_pairing_mode = false;

// Forgets the underlying NimBLE bond (LTK/IRK/CSRK/CCCDs -
// ble_store_config's own NVS-backed store, entirely separate from this
// file's "blepair" slot-to-address mapping) for whoever was in this slot
// before, not just our own record of it - otherwise this device still
// holds stale keys for that peer's address even though
// ble_pair_slot_bonded?() now (correctly) reports the slot as empty.
//
// Called right before activate(slot, true) at each of its call sites
// (not eagerly from switch_or_new() before that) so that if a peer is
// still actively connected on this slot, it sees a normal disconnect
// happen *before* we erase its keys, rather than us ripping out an
// encrypted link's own security material out from under it while it's
// still live. Whether the peer's own OS then actually offers this
// device as a fresh pairing candidate is a separate, peer-side
// question this can't control either way (a BLE peripheral forgetting
// its side doesn't make a central offer to re-pair unless the central
// also forgot it) - this only makes our own side's bookkeeping correct
// and its timing sane.
static void erase_old_bond(int slot)
{
    ble_addr_t old_addr;
    if (load_slot_addr(slot, &old_addr)) {
        ble_store_util_delete_peer(&old_addr);
    }
    erase_slot_addr(slot);
}

static esp_err_t activate(int slot, bool pairing)
{
    ble_gap_adv_stop(); // BLE_HS_EALREADY-ish "wasn't advertising" is expected/harmless here

    const ble_addr_t *own_addr = own_addr_for_slot(slot);

    esp_err_t err;
    if (pairing) {
        err = esp_hid_ble_gap_adv_start(NULL, own_addr);
    } else {
        ble_addr_t addr;
        if (!load_slot_addr(slot, &addr)) {
            return ESP_ERR_NOT_FOUND;
        }
        err = esp_hid_ble_gap_adv_start(&addr, own_addr);
    }
    if (err != ESP_OK) {
        return err;
    }

    s_current_slot = slot;
    s_pairing_mode = pairing;
    if (!pairing) {
        // Pairing mode's peer isn't known yet - ble_pair_slots_on_connect()
        // persists "active" once it actually learns who connected.
        save_active_slot(slot);
    }
    return ESP_OK;
}

void ble_pair_slots_resume_on_start(void)
{
    // A ble_pair_switch()/ble_pair_new() that came in too early (stack
    // started but not yet synced - see switch_or_new()'s own comment)
    // takes priority over whatever slot was last persisted as active.
    if (s_pending_slot >= 0) {
        int slot = s_pending_slot;
        bool pairing = s_pending_pairing_mode;
        s_pending_slot = -1;
        if (pairing) {
            erase_old_bond(slot);
        }
        esp_err_t err = activate(slot, pairing);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "activating slot %d (queued before the stack was ready) failed: %s",
                     slot, esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "slot %d (%s) now that the stack is ready", slot, pairing ? "pairing" : "directed advertising");
        }
        return;
    }

    int slot = load_active_slot();
    if (slot < 0) {
        ESP_LOGI(TAG, "no previously-active slot - staying idle until ble_pair_switch/ble_pair_new is called");
        s_current_slot = -1;
        s_pairing_mode = false;
        return;
    }
    esp_err_t err = activate(slot, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "resuming slot %d on start failed: %s", slot, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "resuming slot %d (directed advertising)", slot);
    }
}

static esp_err_t switch_or_new(int slot, bool pairing)
{
    if (slot < 1 || slot > BLE_PAIR_SLOT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ble_hid_device_started()) {
        ESP_LOGW(TAG, "BLE stack not started - call ble_toggle true first");
        return ESP_ERR_INVALID_STATE;
    }
    if (!pairing && !ble_pair_slot_bonded(slot)) {
        ESP_LOGW(TAG, "slot %d has no bonded device yet - use ble_pair_new(%d) instead", slot, slot);
        return ESP_ERR_NOT_FOUND;
    }
    if (!ble_hid_device_ready()) {
        // Stack is launching but the NimBLE host hasn't finished syncing
        // with the controller yet (real-hardware repro: `ble_toggle true`
        // immediately followed by this, same script tick - an advertising
        // HCI command sent this early fails outright: "ble_hs_hci_cmd_send_buf
        // rc=22"). Queue it - ble_pair_slots_resume_on_start() (called
        // once ESP_HIDD_START_EVENT actually fires) picks this up instead
        // of whatever was last persisted as active.
        s_pending_slot = slot;
        s_pending_pairing_mode = pairing;
        ESP_LOGI(TAG, "BLE stack not synced yet - queuing slot %d (%s) for once it's ready",
                 slot, pairing ? "pairing" : "directed advertising");
        return ESP_OK;
    }
    if (ble_hid_device_connected()) {
        // Can't switch out from under a live connection synchronously -
        // ble_pair_slots_on_disconnect() picks this up and finishes the
        // job once the disconnect it kicks off here actually completes.
        s_pending_slot = slot;
        s_pending_pairing_mode = pairing;
        ble_hid_device_disconnect_current();
        return ESP_OK;
    }
    if (pairing) {
        erase_old_bond(slot);
    }
    return activate(slot, pairing);
}

esp_err_t ble_pair_switch(int slot)
{
    return switch_or_new(slot, false);
}

// Two things this function tried and backed out of, both documented in
// mds/usb_hid/2026-09-12_after_timer_dsl.md's follow-up:
//
// - Bouncing a reconnect from the exact peer erase_old_bond() just forgot
//   (an "avoid this address" check in ble_pair_slots_on_connect()), meant
//   to stop that peer's own OS from silently re-grabbing the slot before
//   a genuinely different device gets a chance. Reverted at the user's
//   call after testing - the underlying peer-side behavior it was
//   chasing (a device that was *just* bonded trying to reconnect with
//   its remembered LTK instead of re-pairing, failing encryption) turned
//   out not to even go through this check in practice, so it added
//   complexity without a clear win, and the simpler behavior below was
//   judged the more tolerable one to live with.
// - Having this function do a full ble_hid_device_stop()+start()
//   internally (the same full BT controller/NimBLE host teardown a
//   manual `ble_toggle false` then `true` does), since that's what
//   real-hardware testing found actually let a fresh pairing succeed
//   reliably. Crashed the board outright - task watchdog on IDLE0, then
//   "assert failed: xQueueSemaphoreTake queue.c:1713 (pxQueue->uxItemSize
//   == 0)" moments into the fresh BLE_INIT, even with a 300ms delay
//   between stop() and start() (confirmed via log timestamps that the
//   delay really did elapse). Reverted rather than keep guessing at
//   bigger delays against hardware that was actively crash-looping.
//
// Net effect: just erase the old bond + restart undirected advertising
// (below), same as before either of those. Known consequence: if the
// peer was *just* bonded here moments earlier, a manual `ble_toggle
// false` -> `true` -> `ble_pair_new()` (as separate calls, with real
// time passing between them) remains the reliable path when a plain
// ble_pair_new() alone doesn't get a clean pairing.
esp_err_t ble_pair_new(int slot)
{
    return switch_or_new(slot, true);
}

int ble_pair_current_slot(void)
{
    return s_current_slot;
}

bool ble_pair_slot_bonded(int slot)
{
    if (slot < 1 || slot > BLE_PAIR_SLOT_COUNT) {
        return false;
    }
    ble_addr_t addr;
    return load_slot_addr(slot, &addr);
}

void ble_pair_slots_forget_all(void)
{
    for (int slot = 1; slot <= BLE_PAIR_SLOT_COUNT; slot++) {
        erase_slot_addr(slot);
    }
    save_active_slot(-1);
    s_current_slot = -1;
    s_pairing_mode = false;
    s_pending_slot = -1;
}

// Tried and reverted: refusing to record a peer here if it was already
// bonded to a *different* slot (real-hardware symptom that was chasing:
// ble_pair_new() on an empty slot opens *undirected* advertising, which
// any device can see - including one already bonded to a different slot
// and nearby actively trying to reconnect on its own). Reverted because
// this cuts both ways with no way to tell which is intended from a bare
// GAP connect event: it also permanently blocks *deliberately* re-pairing
// a device that's already bonded to some other slot to a new one (real
// hardware: a PC already bonded to slot 2 could then never pair to slot
// 1 either, bounced every time with a spurious "encryption failed" -
// mds/usb_hid/2026-09-12_after_timer_dsl.md's follow-up). Undirected
// pairing mode being "whoever connects first, wins" is accepted as an
// inherent limitation now - get other already-bonded devices out of
// range (or turn their Bluetooth off) before opening a new slot for
// pairing if this is a problem in practice.
void ble_pair_slots_on_connect(const ble_addr_t *peer_addr)
{
    if (s_current_slot < 0) {
        return; // shouldn't happen (nothing should be advertising while idle) - defensive only
    }
    if (s_pairing_mode) {
        save_slot_addr(s_current_slot, peer_addr);
        save_active_slot(s_current_slot);
        s_pairing_mode = false;
        ESP_LOGI(TAG, "slot %d bonded to a new device", s_current_slot);
    }
}

void ble_pair_slots_on_disconnect(void)
{
    if (s_pending_slot >= 0) {
        int slot = s_pending_slot;
        bool pairing = s_pending_pairing_mode;
        s_pending_slot = -1;
        if (pairing) {
            erase_old_bond(slot);
        }
        esp_err_t err = activate(slot, pairing);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "activating slot %d (pending switch/new) failed: %s", slot, esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "switched to slot %d (%s)", slot, pairing ? "pairing" : "directed advertising");
        }
        return;
    }

    // Keep chasing the same slot/mode automatically regardless of *why*
    // this disconnected - deliberate (the peer's own Bluetooth settings)
    // or involuntary (out of range) alike, same as an ordinary BLE
    // peripheral would. An earlier version of this function went idle
    // (stopped advertising to anyone) specifically on a deliberate
    // disconnect - walked back (mds/usb_hid/2026-09-12_ble_multi_pair.md's
    // follow-up) because it fought the actual intent once slots existed:
    // the user wants the same PC to be able to reconnect on its own after
    // disconnecting it, and only an explicit ble_pair_switch()/
    // ble_pair_new() call (handled above via s_pending_slot, which
    // *does* disconnect-and-redirect) should ever move to a different
    // device.
    if (s_current_slot >= 0) {
        esp_err_t err = activate(s_current_slot, s_pairing_mode);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "re-advertising slot %d after disconnect failed: %s",
                     s_current_slot, esp_err_to_name(err));
        }
    }
}
