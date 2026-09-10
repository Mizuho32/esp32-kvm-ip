#include "hid_forwarder.h"

#include <stdbool.h>
#include <string.h>
#include <errno.h>

#include "esp_log.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "usb/hid_usage_keyboard.h"

#include "protocol.h"
#include "usb_device_typec.h"
#include "wifi_credentials.h"

// filter_rules.h is a gitignored personal copy of filter_rules.h.example
// (like wifi_credentials.h) - fall back to the tracked, pure-passthrough
// default if it hasn't been created.
#if __has_include("filter_rules.h")
#include "filter_rules.h"
#else
#include "filter_rules_default.h"
#endif

// route_rules.h: same personal-copy-or-default pattern, but for UDP vs
// type-c routing (Phase2, mds/usb_hid/2026-08-23_filter_conv_router_with_max3421.md)
// rather than filtering/remapping - only consulted while type-c is
// actually connected (usb_device_typec_connected()).
#if __has_include("route_rules.h")
#include "route_rules.h"
#else
#include "route_rules_default.h"
#endif

// mruby_filter_active() picks between this file (filter_rules.h/route_rules.h)
// and an embedded/uploaded mruby script's source/sink/pipeline DSL for
// every report - see mds/usb_hid/2026-08-28_mruby_filter_route.md and
// mds/usb_hid/2026-08-29_mruby_phase1_impl.md. filter_rules.h/route_rules.h
// stay fully wired up as the always-available C fallback, not dead code.
#include "mruby_filter.h"

// EXPERIMENTAL performance-isolation toggle (mds/usb_hid/2026-09-07_ble_hid_sink_impl.md's
// follow-up: is BLE mouse FPS actually capped by BLE's own connection-
// interval floor, or is something else in this project's stack - mruby's
// per-report dispatch, WiFi/lwIP running concurrently, etc. - still
// costing something on top of that?). When 1, every keyboard/mouse/
// consumer report goes STRAIGHT to ble_hid_device_*_report(), bypassing
// mruby_filter_active()/the DSL pipeline, UDP, and type-c entirely - the
// shortest possible path from "USB Host backend parsed a report" to "BLE
// notify attempted". Same idiom as HOST_MINIMAL_TEST (main_host.c) /
// BRIDGE_MINIMAL_TEST (usb_host_rp2040_bridge.c) - a separate toggle in
// each file, meant to be flipped together, not a shared header: set
// main_host.c's matching HOST_BLE_ONLY_TEST too, which skips WiFi/WebUI/
// type-c/mruby_filter_init() entirely so nothing else is running
// concurrently either.
#define HOST_BLE_ONLY_TEST 0

#if HOST_BLE_ONLY_TEST
#include "ble_hid_device.h"
#endif

#define TAG "HIDFWD"

static int s_sock = -1;
static struct sockaddr_in s_target_addr;
static uint32_t s_seq;

// Merged keyboard state: a physical keyboard's own keys and any
// mouse-triggered synthetic keys (e.g. back/forward -> Alt+arrow, see
// filter_rules.h/mruby's mouse_synth_keys hook) both contribute to one
// combined report - whichever backend/device sent last would otherwise
// silently overwrite the other's held keys (the UDP protocol carries
// full state, not deltas).
static uint8_t s_kbd_modifiers;
static uint8_t s_kbd_keycodes[6];
static uint8_t s_synth_modifiers;
static uint8_t s_synth_keycode = HID_KEY_NO_PRESS;

static bool resolve_target(void)
{
    struct addrinfo hints = {
        .ai_family   = AF_INET,
        .ai_socktype = SOCK_DGRAM,
    };
    struct addrinfo *res = NULL;
    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%d", UDP_PORT);

    int err = getaddrinfo(KVM_TARGET_HOST, port_str, &hints, &res);
    if (err != 0 || res == NULL) {
        ESP_LOGE(TAG, "Failed to resolve KVM_TARGET_HOST '%s': %d", KVM_TARGET_HOST, err);
        return false;
    }
    memcpy(&s_target_addr, res->ai_addr, sizeof(s_target_addr));
    freeaddrinfo(res);
    return true;
}

static void send_udp_packet_to(const udp_packet_t *pkt, const struct sockaddr_in *dest)
{
    if (s_sock < 0) {
        return;
    }
    sendto(s_sock, pkt, PACKET_SIZE, 0, (struct sockaddr *)dest, sizeof(*dest));
}

void hid_forwarder_send_keyboard_to(const struct sockaddr_in *dest, uint8_t modifiers, const uint8_t keycodes[6])
{
    udp_packet_t pkt = {
        .magic    = PACKET_MAGIC,
        .sequence = ++s_seq,
        .type     = EVENT_TYPE_KEYBOARD,
    };
    pkt.keyboard.modifiers = modifiers;
    pkt.keyboard.reserved  = 0;
    memcpy(pkt.keyboard.keycodes, keycodes, 6);
    send_udp_packet_to(&pkt, dest);
}

void hid_forwarder_send_mouse_to(const struct sockaddr_in *dest, uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan)
{
    udp_packet_t pkt = {
        .magic    = PACKET_MAGIC,
        .sequence = ++s_seq,
        .type     = EVENT_TYPE_MOUSE,
    };
    pkt.mouse.buttons = buttons;
    pkt.mouse.dx      = dx;
    pkt.mouse.dy      = dy;
    pkt.mouse.wheel   = wheel;
    pkt.mouse.pan     = pan;
    send_udp_packet_to(&pkt, dest);
}

void hid_forwarder_send_consumer_to(const struct sockaddr_in *dest, uint16_t usage_id)
{
    udp_packet_t pkt = {
        .magic    = PACKET_MAGIC,
        .sequence = ++s_seq,
        .type     = EVENT_TYPE_CONSUMER,
    };
    pkt.consumer.usage_id = usage_id;
    send_udp_packet_to(&pkt, dest);
}

void hid_forwarder_send_system_control_to(const struct sockaddr_in *dest, uint16_t usage_id)
{
    udp_packet_t pkt = {
        .magic    = PACKET_MAGIC,
        .sequence = ++s_seq,
        .type     = EVENT_TYPE_SYSTEM_CONTROL,
    };
    pkt.system_control.usage_id = usage_id;
    send_udp_packet_to(&pkt, dest);
}

static void send_keyboard_report_raw(uint8_t modifiers, const uint8_t keycodes[6])
{
    hid_forwarder_send_keyboard_to(&s_target_addr, modifiers, keycodes);
}

static void send_mouse_report_raw(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan)
{
    hid_forwarder_send_mouse_to(&s_target_addr, buttons, dx, dy, wheel, pan);
}

static void send_consumer_report_raw(uint16_t usage_id)
{
    hid_forwarder_send_consumer_to(&s_target_addr, usage_id);
}

static void compute_merged_keyboard_report(uint8_t *modifiers, uint8_t keycodes[6])
{
    *modifiers = s_kbd_modifiers | s_synth_modifiers;
    memcpy(keycodes, s_kbd_keycodes, 6);

    if (s_synth_keycode != HID_KEY_NO_PRESS) {
        bool already_present = false;
        for (int i = 0; i < 6; i++) {
            if (keycodes[i] == s_synth_keycode) {
                already_present = true;
                break;
            }
        }
        if (!already_present) {
            for (int i = 0; i < 6; i++) {
                if (keycodes[i] == HID_KEY_NO_PRESS) {
                    keycodes[i] = s_synth_keycode;
                    break;
                }
            }
        }
    }
}

// While type-c is connected (usb_device_typec_connected()), every report
// goes through either the mruby :keyboard pipeline (which owns deciding
// every sink itself, typec included) or, without mruby, straight to
// typec plus whatever route_rules.h decides for UDP. Otherwise (not
// connected, or no MAX3421E/RP2040 bridge at all - see main_host.c)
// everything goes over UDP as before Phase2 - see
// mds/usb_hid/2026-08-23_filter_conv_router_with_max3421.md.
static void dispatch_merged_keyboard_report(void)
{
    uint8_t modifiers;
    uint8_t keycodes[6];
    compute_merged_keyboard_report(&modifiers, keycodes);

    if (mruby_filter_active()) {
        // mruby's :keyboard pipeline owns every sink itself (typec/udp/ble -
        // see mds/usb_hid/2026-09-07_ble_hid_sink_plan.md) and has nothing
        // to do with whether type-c happens to be connected to a real PC
        // right now - a :udp or :ble-only sink shouldn't depend on it at
        // all. This used to be nested inside the usb_device_typec_connected()
        // branch below (a leftover from the pre-mruby design, where
        // type-c was the only sink that ever existed), which silently
        // skipped the whole pipeline - and so any UDP/BLE sink too -
        // whenever type-c was "power only" (plugged in but not enumerated
        // by a PC) or not connected at all. Found via
        // mds/usb_hid/2026-09-07_ble_hid_sink_impl.md's follow-up: BLE
        // reports only ever went out while also enumerated to a PC over
        // type-c. usb_device_typec_keyboard_report() itself stays a safe,
        // immediate no-op when disconnected (wait_for_ready() bails out
        // without blocking), so calling into the pipeline unconditionally
        // costs nothing extra for a typec-routed script either.
        mruby_dispatch_keyboard(modifiers, keycodes);
        return;
    }

    if (usb_device_typec_connected()) {
        usb_device_typec_keyboard_report(modifiers, keycodes);
        if (!route_keyboard_also_udp(modifiers, keycodes)) {
            return;
        }
    }
    send_keyboard_report_raw(modifiers, keycodes);
}

static void apply_mouse_synth_keys(uint8_t modifiers, uint8_t keycode)
{
    if (modifiers == s_synth_modifiers && keycode == s_synth_keycode) {
        return; // No change - avoid a redundant packet on every mouse report.
    }
    s_synth_modifiers = modifiers;
    s_synth_keycode   = keycode;
    dispatch_merged_keyboard_report();
}

void hid_forwarder_keyboard_report(uint8_t modifiers, const uint8_t keycodes_in[6])
{
#if HOST_BLE_ONLY_TEST
    ble_hid_device_keyboard_report(modifiers, keycodes_in);
    return;
#endif
    uint8_t keycodes[6];
    memcpy(keycodes, keycodes_in, 6);

    if (mruby_filter_active()) {
        // Pipeline model: tracking "what's currently held" is unconditional
        // and always reflects the true physical state - remapping/dropping
        // individual keys is a per-sink routing concern handled inside the
        // :keyboard pipeline's `to` blocks (mruby_dispatch_keyboard(), via
        // dispatch_merged_keyboard_report() below), not a global forward/
        // drop gate here anymore. See mds/usb_hid/2026-08-29_mruby_phase1_impl.md.
        s_kbd_modifiers = modifiers;
        memcpy(s_kbd_keycodes, keycodes, 6);
        dispatch_merged_keyboard_report();
        return;
    }

    if (filter_keyboard_report(&modifiers, keycodes)) {
        s_kbd_modifiers = modifiers;
        memcpy(s_kbd_keycodes, keycodes, 6);
        dispatch_merged_keyboard_report();
    }
}

void hid_forwarder_mouse_sample(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan)
{
#if HOST_BLE_ONLY_TEST
    ble_hid_device_mouse_report(buttons, dx, dy, wheel, pan);
    return;
#endif
    uint8_t synth_modifiers = 0;
    uint8_t synth_keycode = HID_KEY_NO_PRESS;

    if (mruby_filter_active()) {
        // mruby_dispatch_mouse() owns every sink (typec and any named
        // UDP/BLE sinks) via the :mouse pipeline's to/branch stages, and
        // fills synth_modifiers/synth_keycode via the script's optional
        // mouse_synth_keys hook (see mds/usb_hid/2026-08-29_mruby_phase1_impl.md).
        // See dispatch_merged_keyboard_report()'s comment - this no longer
        // depends on usb_device_typec_connected() for the same reason.
        mruby_dispatch_mouse(buttons, dx, dy, wheel, pan, &synth_modifiers, &synth_keycode);
    } else if (usb_device_typec_connected()) {
        // Split (rough, kept for the C fallback path): filter_rules.h
        // only shapes the type-c-bound copy; route_rules.h/UDP always
        // see the original raw values, never the filtered ones - see
        // mds/usb_hid/2026-08-21_filter_conv_route.md.
        uint8_t f_buttons = buttons;
        int16_t f_dx = dx, f_dy = dy;
        int8_t f_wheel = wheel, f_pan = pan;
        bool forward_typec = filter_mouse_report(&f_buttons, &f_dx, &f_dy, &f_wheel, &f_pan,
                                                  &synth_modifiers, &synth_keycode);
        if (forward_typec) {
            usb_device_typec_mouse_report(f_buttons, f_dx, f_dy, f_wheel, f_pan);
        }
        if (route_mouse_also_udp(buttons, dx, dy, wheel, pan)) {
            send_mouse_report_raw(buttons, dx, dy, wheel, pan);
        }
    } else {
        send_mouse_report_raw(buttons, dx, dy, wheel, pan);
    }
    apply_mouse_synth_keys(synth_modifiers, synth_keycode);
}

void hid_forwarder_consumer(uint16_t usage_id)
{
#if HOST_BLE_ONLY_TEST
    ble_hid_device_consumer_report(usage_id);
    return;
#endif
    // See dispatch_merged_keyboard_report()'s comment - same reasoning.
    if (mruby_filter_active()) {
        mruby_dispatch_consumer(usage_id);
        return;
    }

    if (usb_device_typec_connected()) {
        usb_device_typec_consumer_report(usage_id);
        if (!route_consumer_also_udp(usage_id)) {
            return;
        }
    }
    send_consumer_report_raw(usage_id);
}

esp_err_t hid_forwarder_init(void)
{
    memset(s_kbd_keycodes, HID_KEY_NO_PRESS, sizeof(s_kbd_keycodes));

    if (!resolve_target()) {
        return ESP_FAIL;
    }

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "Failed to create UDP socket: errno %d", errno);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Forwarding USB HID input to %s:%d", KVM_TARGET_HOST, UDP_PORT);
    return ESP_OK;
}
