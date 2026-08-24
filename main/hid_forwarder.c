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
// type-c routing (Phase2, mds/2026-08-23_filter_conv_router_with_max3421.md)
// rather than filtering/remapping - only consulted while type-c is
// actually connected (usb_device_typec_connected()).
#if __has_include("route_rules.h")
#include "route_rules.h"
#else
#include "route_rules_default.h"
#endif

#define TAG "HIDFWD"

static int s_sock = -1;
static struct sockaddr_in s_target_addr;
static uint32_t s_seq;

// Merged keyboard state: a physical keyboard's own keys and any
// mouse-triggered synthetic keys (e.g. back/forward -> Alt+arrow, see
// filter_rules.h) both contribute to one combined report - whichever
// backend/device sent last would otherwise silently overwrite the
// other's held keys (the UDP protocol carries full state, not deltas).
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

static void send_udp_packet(const udp_packet_t *pkt)
{
    if (s_sock < 0) {
        return;
    }
    sendto(s_sock, pkt, PACKET_SIZE, 0, (struct sockaddr *)&s_target_addr, sizeof(s_target_addr));
}

static void send_keyboard_report_raw(uint8_t modifiers, const uint8_t keycodes[6])
{
    udp_packet_t pkt = {
        .magic    = PACKET_MAGIC,
        .sequence = ++s_seq,
        .type     = EVENT_TYPE_KEYBOARD,
    };
    pkt.keyboard.modifiers = modifiers;
    pkt.keyboard.reserved  = 0;
    memcpy(pkt.keyboard.keycodes, keycodes, 6);
    send_udp_packet(&pkt);
}

static void send_mouse_report_raw(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan)
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
    send_udp_packet(&pkt);
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
// always goes there; route_rules.h decides whether it *also* gets
// mirrored over UDP. Otherwise (not connected, or no MAX3421E at all -
// see main_host.c) everything goes over UDP as before Phase2 - see
// mds/2026-08-23_filter_conv_router_with_max3421.md.
static void dispatch_merged_keyboard_report(void)
{
    uint8_t modifiers;
    uint8_t keycodes[6];
    compute_merged_keyboard_report(&modifiers, keycodes);

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
    uint8_t keycodes[6];
    memcpy(keycodes, keycodes_in, 6);
    if (filter_keyboard_report(&modifiers, keycodes)) {
        s_kbd_modifiers = modifiers;
        memcpy(s_kbd_keycodes, keycodes, 6);
        dispatch_merged_keyboard_report();
    }
}

void hid_forwarder_mouse_sample(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan)
{
    uint8_t synth_modifiers = 0;
    uint8_t synth_keycode = HID_KEY_NO_PRESS;

    // Split (rough, to be revisited): filter_rules.h now only shapes the
    // type-c-bound copy; route_rules.h/UDP always see the original raw
    // values, never the filtered ones. This lets e.g. wheel be dropped
    // from type-c via filter_rules.h while still reaching the Device-role
    // board over UDP via route_rules.h - see README.md's filter/conv/route
    // section.
    uint8_t f_buttons = buttons;
    int16_t f_dx = dx, f_dy = dy;
    int8_t f_wheel = wheel, f_pan = pan;
    bool forward_typec = filter_mouse_report(&f_buttons, &f_dx, &f_dy, &f_wheel, &f_pan,
                                             &synth_modifiers, &synth_keycode);

    if (usb_device_typec_connected()) {
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
    if (usb_device_typec_connected()) {
        usb_device_typec_consumer_report(usage_id);
        if (!route_consumer_also_udp(usage_id)) {
            return;
        }
    }

    udp_packet_t pkt = {
        .magic    = PACKET_MAGIC,
        .sequence = ++s_seq,
        .type     = EVENT_TYPE_CONSUMER,
    };
    pkt.consumer.usage_id = usage_id;
    send_udp_packet(&pkt);
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
