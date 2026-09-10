#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

#define PACKET_MAGIC   0xCAFE
#define UDP_PORT       4210
#define PACKET_SIZE    16

typedef enum : uint8_t {
    EVENT_TYPE_MOUSE    = 0x01,
    EVENT_TYPE_KEYBOARD = 0x02,
    EVENT_TYPE_CONSUMER = 0x03,
    EVENT_TYPE_SYSTEM_CONTROL = 0x04,
} event_type_t;

// ── Incoming UDP packet from server ──────────────────────────────
typedef struct __attribute__((packed)) {
    uint16_t magic;         // 0xCAFE
    uint32_t sequence;      // Sequence counter
    uint8_t  type;          // event_type_t
    uint8_t  _reserved;     // Padding

    union {
        struct __attribute__((packed)) {
            uint8_t  buttons;   // 5 buttons
            int16_t  dx;        // Delta X
            int16_t  dy;        // Delta Y
            int8_t   wheel;     // Scroll V
            int8_t   pan;       // Scroll H
            uint8_t  _pad;
        } mouse;                // 8 bytes

        struct __attribute__((packed)) {
            uint8_t modifiers;  // Modifiers
            uint8_t reserved;
            uint8_t keycodes[6];// 6-key rollover
        } keyboard;             // 8 bytes

        struct __attribute__((packed)) {
            uint16_t usage_id;  // Consumer Usage ID (0 = release)
            uint8_t  _pad[6];
        } consumer;             // 8 bytes

        struct __attribute__((packed)) {
            // Generic Desktop page (0x01) System Control usage ID (0x81
            // Power Down / 0x82 Sleep / 0x83 Wake Up, 0 = idle/release) -
            // see mds/usb_hid/2026-09-10_system_control_sleep.md. Same
            // "raw HID usage ID, mapped to the wire report's actual
            // encoding only at the last hop" convention as `consumer`
            // above - see usb_device_typec_system_control_report()/
            // ble_hid_device_system_control_report().
            uint16_t usage_id;
            uint8_t  _pad[6];
        } system_control;       // 8 bytes
    };
} udp_packet_t;

_Static_assert(sizeof(udp_packet_t) == PACKET_SIZE,
               "Packet must be exactly 16 bytes");

// ── Internal event (passed via xQueue) ─────────────────────────
typedef struct {
    event_type_t type;
    union {
        struct {
            uint8_t buttons;
            int16_t dx;
            int16_t dy;
            int8_t  wheel;
            int8_t  pan;
        } mouse;
        struct {
            uint8_t modifiers;
            uint8_t keycodes[6];
        } keyboard;
        struct {
            uint16_t usage_id;
        } consumer;
        struct {
            uint16_t usage_id;
        } system_control;
    };
} hid_event_t;

#endif
