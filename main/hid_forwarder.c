#include "hid_forwarder.h"

#include <stdbool.h>
#include <string.h>
#include <errno.h>

#include "esp_log.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "usb/hid_usage_keyboard.h"

#include "protocol.h"
#include "wifi_credentials.h"

// filter_rules.h is a gitignored personal copy of filter_rules.h.example
// (like wifi_credentials.h) - fall back to the tracked, pure-passthrough
// default if it hasn't been created.
#if __has_include("filter_rules.h")
#include "filter_rules.h"
#else
#include "filter_rules_default.h"
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

static void send_merged_keyboard_report(void)
{
    uint8_t modifiers = s_kbd_modifiers | s_synth_modifiers;
    uint8_t keycodes[6];
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

    send_keyboard_report_raw(modifiers, keycodes);
}

static void apply_mouse_synth_keys(uint8_t modifiers, uint8_t keycode)
{
    if (modifiers == s_synth_modifiers && keycode == s_synth_keycode) {
        return; // No change - avoid a redundant packet on every mouse report.
    }
    s_synth_modifiers = modifiers;
    s_synth_keycode   = keycode;
    send_merged_keyboard_report();
}

void hid_forwarder_keyboard_report(uint8_t modifiers, const uint8_t keycodes_in[6])
{
    uint8_t keycodes[6];
    memcpy(keycodes, keycodes_in, 6);
    if (filter_keyboard_report(&modifiers, keycodes)) {
        s_kbd_modifiers = modifiers;
        memcpy(s_kbd_keycodes, keycodes, 6);
        send_merged_keyboard_report();
    }
}

void hid_forwarder_mouse_sample(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan)
{
    uint8_t synth_modifiers = 0;
    uint8_t synth_keycode = HID_KEY_NO_PRESS;

    if (filter_mouse_report(&buttons, &dx, &dy, &wheel, &pan, &synth_modifiers, &synth_keycode)) {
        send_mouse_report_raw(buttons, dx, dy, wheel, pan);
    }
    apply_mouse_synth_keys(synth_modifiers, synth_keycode);
}

void hid_forwarder_consumer(uint16_t usage_id)
{
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
