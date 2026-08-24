#include "usb_host_rp2040_bridge.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "class/hid/hid.h" // hid_keyboard_report_t / hid_mouse_report_t only - no tuh_*/tud_* dependency

#include "hid_forwarder.h"
#include "hid_report_parser.h"

#define TAG "USBHOST_RP2040BRIDGE"

// Actual wiring - adjust to match yours, same as usb_host_max3421.c's
// MAX3421_PIN_*. GPIO17/18 don't exist on ESP32-S3 (only up to ~GPIO21,
// with some numbers skipped) - moved to GPIO5/6, which happens to
// overlap MAX3421_PIN_RST/MAX3421_PIN_INT in usb_host_max3421.c. That's
// fine: only one Host backend is ever wired/running at a time (see
// main_host.c) - this is no longer meant to coexist with a wired-up
// MAX3421E on the same board.
#define BRIDGE_UART_PORT  UART_NUM_1
#define BRIDGE_UART_TX_PIN 5
#define BRIDGE_UART_RX_PIN 6
#define BRIDGE_UART_BAUD  460800

// Frame format (see mds/usb_hid/2026-08-23_rp2040_as_host_bridge_plan.md):
//   [0xAA sync][msg_type][dev_addr][idx][itf_protocol][len_lo][len_hi][payload...][checksum]
// checksum = XOR of every byte from msg_type through the last payload
// byte (i.e. everything except the sync byte itself). A byte stream, not
// a shared-clock multi-byte SPI transaction, so - unlike MAX3421 - this
// doesn't need precise timing; a corrupted/dropped byte just fails the
// checksum and the parser resyncs on the next 0xAA, rather than wedging
// the whole link.
#define BRIDGE_SYNC_BYTE 0xAA

typedef enum {
    BRIDGE_MSG_HEARTBEAT = 0x01, // len=0 - sent periodically regardless of USB device state, used for probing
    BRIDGE_MSG_MOUNT     = 0x02, // payload = HID Report Descriptor
    BRIDGE_MSG_UNMOUNT   = 0x03, // len=0
    BRIDGE_MSG_REPORT    = 0x04, // payload = raw HID report bytes
    // RP2040-side RATE_MONITOR stats (rp2040_host_bridge.ino), sent once
    // a second over this same link so they show up here without a
    // separate USB-serial adapter wired to the RP2040's Serial2 - see
    // mds/usb_hid/2026-08-24_rp2040_bridge_fps_investigation.md. Payload: 3x
    // uint32 LE (reports_per_sec, min_interval_us, max_interval_us).
    BRIDGE_MSG_STATS     = 0x05,
} bridge_msg_type_t;

// USB HID spec bInterfaceProtocol values (not a TinyUSB-specific enum -
// this file deliberately doesn't include any tuh_*/tud_* header, only
// class/hid/hid.h for the two Boot Protocol report structs). The RP2040
// side sends these verbatim from its own tuh_hid_interface_protocol().
#define ITF_PROTOCOL_NONE     0
#define ITF_PROTOCOL_KEYBOARD 1
#define ITF_PROTOCOL_MOUSE    2

#define MAX_PAYLOAD_LEN 512

// Toggle for a report-rate/checksum-failure counter, printed once a
// second via ESP_LOGI (mds/usb_hid/2026-08-24_rp2040_bridge_fps_investigation.md
// measurement plan, point 2). The RP2040 side's own RATE_MONITOR/
// POLL_CEILING_TEST confirmed it emits a clean ~100Hz - this counts how
// many REPORT frames actually reach dispatch_report() here per second,
// and how many frames get thrown away by a checksum mismatch, to tell
// whether the UART link between the two boards is where anything gets
// lost/delayed. A once-a-second summary line, not a per-frame dump -
// the per-frame raw-report dump in dispatch_report() below is what
// perturbed timing while chasing the type-c crash
// (mds/usb_hid/2026-08-23_rp2040_host_status.md); this shouldn't have that
// problem.
// Back to 1 - disabling this entirely (mds/usb_hid/2026-08-24_rp2040_bridge_fps_investigation.md)
// did NOT change the choppy-cursor symptom, ruling out "the diagnostic
// logging itself is the cause". Re-enabled to keep visibility while
// testing BRIDGE_MINIMAL_TEST below.
#define BRIDGE_RATE_MONITOR 0

// When 1: usb_host_rp2040_bridge_task_start() only starts bridge_task
// (UART parsing + BRIDGE_RATE_MONITOR's counters), not dispatch_task -
// isolates whether the bursty delivery survives with genuinely nothing
// else from this file running. Combine with main_host.c's
// HOST_MINIMAL_TEST to also strip out WiFi/hid_forwarder/type-c
// entirely, to test whether *those* (not this file) are what's
// starving bridge_task.
#define BRIDGE_MINIMAL_TEST 0

#if BRIDGE_RATE_MONITOR
static volatile uint32_t s_report_count;
static volatile uint32_t s_checksum_fail_count;
static volatile uint32_t s_queue_drop_count;
// Every other counter here is a per-second average - none of them can
// tell bursty delivery (several reports arriving almost back-to-back,
// then a longer gap) from perfectly even ~10ms-spaced delivery, even
// though both would average to the same ~100/sec. A visibly choppy
// cursor despite every measured stage succeeding (0 checksum failures,
// 0 queue drops, 0 tud_hid_n_report() submit failures - see
// usb_device_typec.c) is exactly what bursty delivery would look like:
// several reports processed and submitted within a few ms of each
// other (indistinguishable from healthy here), followed by a gap far
// longer than the nominal ~10ms period. Tracks the shortest gap between
// two consecutive REPORT frames reaching here, reset every print window.
static int64_t  s_last_report_time_us;
static uint32_t s_min_interval_us;
static uint32_t s_max_interval_us; // the "quiet gap" size, if delivery is bursty
#endif

// dispatch_mount()/dispatch_report()/etc. below can block for a while -
// dispatch_report() -> hid_forwarder_mouse_sample() ->
// usb_device_typec_mouse_report() -> wait_for_ready() blocks on
// tud_hid_n_ready() whenever type-c is connected but the PC hasn't
// polled the IN endpoint yet (usb_device_typec.c). Measuring
// BRIDGE_RATE_MONITOR's counters at the *point where handle_frame()
// used to call dispatch_*() directly* (i.e. from the same task that
// also calls uart_read_bytes()) showed the reports/sec actually
// reaching here dropping far below what the RP2040 side independently
// measured itself sending (~100Hz, 0 drops -
// mds/usb_hid/2026-08-24_rp2040_bridge_fps_investigation.md) - with zero
// checksum failures the whole time. That combination means bytes were
// being lost *before* ever reaching feed_byte()'s state machine (a
// checksum failure only fires for a byte stream that did reach the
// parser but didn't match) - i.e. the fixed-size UART RX ring buffer
// (uart_driver_install() above) was overflowing and silently dropping
// incoming bytes at the driver/hardware level while this task sat
// blocked inside dispatch_report() instead of calling
// uart_read_bytes() again.
//
// Fix: decouple parsing (bridge_task(), below - always keeps draining
// UART, never calls into dispatch_*()) from forwarding (dispatch_task(),
// further below - does the actual, possibly-blocking work) via a queue.
// If the queue is ever full (dispatch genuinely can't keep up),
// xQueueSend()'s 0 timeout just drops that one new frame rather than
// blocking the sender - see s_queue_drop_count above - which only loses
// forwarded HID data, never corrupts/desyncs the raw byte stream
// feeding the parser.
typedef struct {
    uint8_t  msg_type;
    uint8_t  dev_addr;
    uint8_t  idx;
    uint8_t  itf_protocol;
    uint16_t len;
    uint8_t  payload[MAX_PAYLOAD_LEN];
} bridge_frame_t;

#define FRAME_QUEUE_DEPTH 8
static QueueHandle_t s_frame_queue;

static bool s_uart_initialized;

static esp_err_t bridge_uart_init(void)
{
    if (s_uart_initialized) {
        return ESP_OK;
    }
    // Created here (rather than in usb_host_rp2040_bridge_task_start())
    // so it exists even during usb_host_rp2040_bridge_probe() - the probe
    // runs feed_byte() (and thus handle_frame(), which calls
    // xQueueSend()) directly, before task_start() would otherwise have
    // created it, and a re-announced MOUNT (or an actual REPORT, if the
    // user happens to move the mouse during the probe's 800ms window)
    // can arrive during that window, not just HEARTBEATs.
    if (!s_frame_queue) {
        s_frame_queue = xQueueCreate(FRAME_QUEUE_DEPTH, sizeof(bridge_frame_t));
        if (!s_frame_queue) {
            return ESP_ERR_NO_MEM;
        }
    }
    uart_config_t cfg = {
        .baud_rate = BRIDGE_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    esp_err_t err = uart_driver_install(BRIDGE_UART_PORT, 1024, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_param_config(BRIDGE_UART_PORT, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_set_pin(BRIDGE_UART_PORT, BRIDGE_UART_TX_PIN, BRIDGE_UART_RX_PIN,
                        UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        return err;
    }
    s_uart_initialized = true;
    return ESP_OK;
}

// ── Per-device state, purely for dispatch/parsing - mirrors
// usb_host_max3421.c's tables exactly, except itf_protocol is stored
// explicitly (sent by the RP2040 in every frame) instead of re-queried
// via tuh_hid_itf_get_info(), which doesn't exist on this transport - no
// TinyUSB Host stack runs on this side of the link at all. ──

#define MAX_MOUSE_DEVICES 4
typedef struct {
    uint8_t                  dev_addr;
    uint8_t                  idx;
    bool                     use_report_protocol;
    mouse_report_layout_t    layout;
    consumer_report_layout_t consumer_layout;
} bridge_mouse_state_t;
static bridge_mouse_state_t s_mouse_devices[MAX_MOUSE_DEVICES];
static int s_mouse_device_count;

#define MAX_CONSUMER_DEVICES 4
typedef struct {
    uint8_t                  dev_addr;
    uint8_t                  idx;
    consumer_report_layout_t layout;
} bridge_consumer_state_t;
static bridge_consumer_state_t s_consumer_devices[MAX_CONSUMER_DEVICES];
static int s_consumer_device_count;

static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static bridge_mouse_state_t *find_mouse_device(uint8_t dev_addr, uint8_t idx)
{
    for (int i = 0; i < s_mouse_device_count; i++) {
        if (s_mouse_devices[i].dev_addr == dev_addr && s_mouse_devices[i].idx == idx) {
            return &s_mouse_devices[i];
        }
    }
    return NULL;
}

static bridge_mouse_state_t *register_mouse_device(uint8_t dev_addr, uint8_t idx)
{
    // Idempotent: the RP2040 side periodically re-announces its
    // currently-mounted devices (so this ESP32 side learns about them
    // even if it reboots mid-session, e.g. reflashing firmware, while
    // RP2040 itself keeps running - see
    // mds/usb_hid/2026-08-23_rp2040_as_host_bridge_plan.md). Without this check,
    // every re-announcement would append a fresh duplicate entry until
    // MAX_MOUSE_DEVICES filled up and further (re-)mounts silently
    // failed.
    bridge_mouse_state_t *existing = find_mouse_device(dev_addr, idx);
    if (existing) {
        memset(existing, 0, sizeof(*existing));
        existing->dev_addr = dev_addr;
        existing->idx      = idx;
        return existing;
    }
    if (s_mouse_device_count >= MAX_MOUSE_DEVICES) {
        return NULL;
    }
    bridge_mouse_state_t *d = &s_mouse_devices[s_mouse_device_count++];
    memset(d, 0, sizeof(*d));
    d->dev_addr = dev_addr;
    d->idx      = idx;
    return d;
}

static void unregister_mouse_device(uint8_t dev_addr, uint8_t idx)
{
    for (int i = 0; i < s_mouse_device_count; i++) {
        if (s_mouse_devices[i].dev_addr == dev_addr && s_mouse_devices[i].idx == idx) {
            s_mouse_devices[i] = s_mouse_devices[--s_mouse_device_count];
            return;
        }
    }
}

static bridge_consumer_state_t *find_consumer_device(uint8_t dev_addr, uint8_t idx)
{
    for (int i = 0; i < s_consumer_device_count; i++) {
        if (s_consumer_devices[i].dev_addr == dev_addr && s_consumer_devices[i].idx == idx) {
            return &s_consumer_devices[i];
        }
    }
    return NULL;
}

static bridge_consumer_state_t *register_consumer_device(uint8_t dev_addr, uint8_t idx)
{
    // Idempotent - see register_mouse_device()'s comment.
    bridge_consumer_state_t *existing = find_consumer_device(dev_addr, idx);
    if (existing) {
        memset(existing, 0, sizeof(*existing));
        existing->dev_addr = dev_addr;
        existing->idx      = idx;
        return existing;
    }
    if (s_consumer_device_count >= MAX_CONSUMER_DEVICES) {
        return NULL;
    }
    bridge_consumer_state_t *d = &s_consumer_devices[s_consumer_device_count++];
    memset(d, 0, sizeof(*d));
    d->dev_addr = dev_addr;
    d->idx      = idx;
    return d;
}

static void unregister_consumer_device(uint8_t dev_addr, uint8_t idx)
{
    for (int i = 0; i < s_consumer_device_count; i++) {
        if (s_consumer_devices[i].dev_addr == dev_addr && s_consumer_devices[i].idx == idx) {
            s_consumer_devices[i] = s_consumer_devices[--s_consumer_device_count];
            return;
        }
    }
}

static void handle_keyboard_report(const uint8_t *data, size_t length)
{
    if (length < sizeof(hid_keyboard_report_t)) {
        return;
    }
    const hid_keyboard_report_t *report = (const hid_keyboard_report_t *)data;
    hid_forwarder_keyboard_report(report->modifier, report->keycode);
}

static void handle_mouse_report_boot(const uint8_t *data, size_t length)
{
    if (length < sizeof(hid_mouse_report_t)) {
        return;
    }
    const hid_mouse_report_t *report = (const hid_mouse_report_t *)data;
    hid_forwarder_mouse_sample(report->buttons, report->x, report->y, 0, 0);
}

static void handle_mouse_report_generic(const mouse_report_layout_t *layout, const uint8_t *data, size_t length)
{
    uint8_t buttons = 0;
    for (uint8_t i = 0; i < layout->button_count && i < HID_MAX_BUTTONS; i++) {
        if (hid_extract_field(data, length, &layout->buttons[i]) != 0) {
            buttons |= (uint8_t)(1u << i);
        }
    }

    int16_t dx = (int16_t)clamp_i32(hid_extract_field(data, length, &layout->x), INT16_MIN, INT16_MAX);
    int16_t dy = (int16_t)clamp_i32(hid_extract_field(data, length, &layout->y), INT16_MIN, INT16_MAX);
    int8_t wheel = layout->wheel.present
                       ? (int8_t)clamp_i32(hid_extract_field(data, length, &layout->wheel), INT8_MIN, INT8_MAX)
                       : 0;
    int8_t pan = layout->pan.present
                     ? (int8_t)clamp_i32(hid_extract_field(data, length, &layout->pan), INT8_MIN, INT8_MAX)
                     : 0;

    hid_forwarder_mouse_sample(buttons, dx, dy, wheel, pan);
}

static void handle_consumer_report(const consumer_report_layout_t *layout, const uint8_t *data, size_t length)
{
    uint16_t usage_id = (uint16_t)hid_extract_field(data, length, &layout->selector);
    hid_forwarder_consumer(usage_id);
}

static void dispatch_mount(uint8_t dev_addr, uint8_t idx, uint8_t itf_protocol,
                          const uint8_t *report_desc, uint16_t desc_len)
{
    // RP2040 re-announces currently-mounted devices every
    // REANNOUNCE_INTERVAL_MS (rp2040_host_bridge.ino, 2s) so an
    // ESP32-side reboot doesn't lose state - register_mouse_device()/
    // register_consumer_device() are idempotent for exactly that reason.
    // Logging (especially the report-descriptor hex dump) used to fire
    // unconditionally on *every* call, including every re-announce, not
    // just the actual first mount - harmless on its own, but once the
    // UART read timeout fix (mds/usb_hid/2026-08-24_rp2040_bridge_fps_investigation.md)
    // made the rest of the pipeline smooth, that synchronous console
    // output every 2s became a noticeable periodic hitch by itself. Only
    // log when the device wasn't already known.
    if (itf_protocol == ITF_PROTOCOL_MOUSE) {
        bool is_new = find_mouse_device(dev_addr, idx) == NULL;
        bridge_mouse_state_t *dev = register_mouse_device(dev_addr, idx);
        if (dev && report_desc && desc_len > 0) {
            hid_parse_mouse_report_descriptor(report_desc, desc_len, &dev->layout);
            dev->use_report_protocol = dev->layout.x.present && dev->layout.y.present;

            hid_parse_consumer_report_descriptor(report_desc, desc_len, &dev->consumer_layout);
            if (is_new) {
                ESP_LOGI(TAG, "HID mounted: dev_addr=%d idx=%d itf_protocol=%d, report descriptor (%d bytes):",
                         dev_addr, idx, itf_protocol, (int)desc_len);
                ESP_LOG_BUFFER_HEX(TAG, report_desc, desc_len);
                if (dev->consumer_layout.selector.present) {
                    ESP_LOGI(TAG, "Mouse also has a bundled Consumer Control selector (report_id=%d bit_length=%d)",
                             dev->consumer_layout.selector.report_id, dev->consumer_layout.selector.bit_length);
                }
                ESP_LOGI(TAG, "Mouse connected (use_report_protocol=%d buttons=%d wheel=%d pan=%d)",
                         dev->use_report_protocol, dev->layout.button_count, dev->layout.wheel.present, dev->layout.pan.present);
            }
        }
    } else if (itf_protocol == ITF_PROTOCOL_NONE) {
        consumer_report_layout_t layout = {0};
        if (report_desc && desc_len > 0) {
            hid_parse_consumer_report_descriptor(report_desc, desc_len, &layout);
        }
        if (layout.selector.present) {
            bool is_new = find_consumer_device(dev_addr, idx) == NULL;
            bridge_consumer_state_t *dev = register_consumer_device(dev_addr, idx);
            if (dev) {
                dev->layout = layout;
                if (is_new) {
                    ESP_LOGI(TAG, "HID mounted: dev_addr=%d idx=%d itf_protocol=%d, report descriptor (%d bytes):",
                             dev_addr, idx, itf_protocol, (int)desc_len);
                    ESP_LOG_BUFFER_HEX(TAG, report_desc, desc_len);
                    ESP_LOGI(TAG, "Consumer Control device connected (media keys): bit_offset=%d bit_length=%d report_id=%d",
                             layout.selector.bit_offset, layout.selector.bit_length, layout.selector.report_id);
                }
            }
        }
    }
    // itf_protocol == ITF_PROTOCOL_KEYBOARD needs no registration - decoded
    // directly as a fixed-layout Boot report in dispatch_report().
}

static void dispatch_umount(uint8_t dev_addr, uint8_t idx)
{
    ESP_LOGI(TAG, "HID unmounted: dev_addr=%d idx=%d", dev_addr, idx);
    unregister_mouse_device(dev_addr, idx);
    unregister_consumer_device(dev_addr, idx);
}

static void dispatch_report(uint8_t dev_addr, uint8_t idx, uint8_t itf_protocol,
                           const uint8_t *report, uint16_t len)
{
    // Debug aid - see usb_host_max3421.c's equivalent toggle. Confirms
    // whether a REPORT frame actually arrived intact over UART (checksum
    // passed) before it gets this far. Toggled OFF (was toggled on while
    // chasing the type-c crash, mds/usb_hid/2026-08-23_rp2040_host_status.md) -
    // suspected contributor to the type-c latency being worse than UDP
    // (synchronous UART0/console log output on every single report).
    /*
    ESP_LOGI(TAG, "[%d:%d] raw report (%d bytes):", dev_addr, idx, (int)len);
    ESP_LOG_BUFFER_HEX(TAG, report, len);
    //*/

    if (itf_protocol == ITF_PROTOCOL_KEYBOARD) {
        handle_keyboard_report(report, len);
    } else if (itf_protocol == ITF_PROTOCOL_MOUSE) {
        bridge_mouse_state_t *dev = find_mouse_device(dev_addr, idx);
        if (dev && dev->consumer_layout.selector.present &&
            dev->consumer_layout.selector.report_id != 0 &&
            len >= 1 && report[0] == dev->consumer_layout.selector.report_id) {
            handle_consumer_report(&dev->consumer_layout, report, len);
        } else if (dev && dev->use_report_protocol) {
            handle_mouse_report_generic(&dev->layout, report, len);
        } else if (dev) {
            handle_mouse_report_boot(report, len);
        }
    } else {
        bridge_consumer_state_t *dev = find_consumer_device(dev_addr, idx);
        if (dev) {
            handle_consumer_report(&dev->layout, report, len);
        }
    }
}

// ── UART frame parser - byte-at-a-time state machine, resyncs on the
// next BRIDGE_SYNC_BYTE whenever a length looks bogus or a checksum
// fails, rather than wedging (see the frame format comment above). ──

typedef enum {
    ST_WAIT_SYNC, ST_TYPE, ST_ADDR, ST_IDX, ST_PROTO, ST_LEN_LO, ST_LEN_HI, ST_PAYLOAD, ST_CHECKSUM,
} parser_state_t;

static parser_state_t s_state = ST_WAIT_SYNC;
static uint8_t  s_msg_type, s_dev_addr, s_idx, s_itf_protocol;
static uint16_t s_len, s_payload_idx;
static uint8_t  s_payload[MAX_PAYLOAD_LEN];
static uint8_t  s_checksum;
static volatile bool s_frame_seen; // set on any validated frame - used by the probe

static void reset_parser(void)
{
    s_state = ST_WAIT_SYNC;
}

static void handle_frame(void)
{
    s_frame_seen = true;
    switch (s_msg_type) {
    case BRIDGE_MSG_HEARTBEAT:
        break; // probing/keepalive only
    case BRIDGE_MSG_STATS: {
        // Diagnostic only - logged directly here (not handed to
        // dispatch_task()) since it's once-a-second and not device HID
        // data. Payload: 3x uint32 LE, see the enum comment above.
        if (s_payload_idx >= 12) {
            uint32_t reports, min_us, max_us;
            memcpy(&reports, &s_payload[0], sizeof(reports));
            memcpy(&min_us, &s_payload[4], sizeof(min_us));
            memcpy(&max_us, &s_payload[8], sizeof(max_us));
            ESP_LOGI(TAG, "[rp2040-rate] %u reports/sec, min interval %uus, max interval %uus",
                     (unsigned)reports, (unsigned)min_us, (unsigned)max_us);
        }
        break;
    }
    case BRIDGE_MSG_MOUNT:
    case BRIDGE_MSG_UNMOUNT:
    case BRIDGE_MSG_REPORT: {
#if BRIDGE_RATE_MONITOR
        if (s_msg_type == BRIDGE_MSG_REPORT) {
            s_report_count++;
            int64_t now_us = esp_timer_get_time();
            if (s_last_report_time_us != 0) {
                int64_t interval = now_us - s_last_report_time_us;
                if (s_min_interval_us == 0 || interval < s_min_interval_us) {
                    s_min_interval_us = (uint32_t)interval;
                }
                if (interval > s_max_interval_us) {
                    s_max_interval_us = (uint32_t)interval;
                }
            }
            s_last_report_time_us = now_us;
        }
#endif
        // Hand off to dispatch_task() rather than calling
        // dispatch_mount()/dispatch_report()/etc. directly here - see the
        // block comment above bridge_frame_t. xQueueSend() with a 0
        // timeout never blocks this (parser) task even if dispatch_task()
        // is itself stuck waiting on wait_for_ready().
        bridge_frame_t frame = {
            .msg_type     = s_msg_type,
            .dev_addr     = s_dev_addr,
            .idx          = s_idx,
            .itf_protocol = s_itf_protocol,
            .len          = s_payload_idx,
        };
        if (s_payload_idx > 0) {
            memcpy(frame.payload, s_payload, s_payload_idx);
        }
        if (xQueueSend(s_frame_queue, &frame, 0) != pdTRUE) {
#if BRIDGE_RATE_MONITOR
            s_queue_drop_count++;
#endif
        }
        break;
    }
    default:
        ESP_LOGW(TAG, "unknown msg_type 0x%02x, ignoring", s_msg_type);
        break;
    }
}

static void feed_byte(uint8_t b)
{
    switch (s_state) {
    case ST_WAIT_SYNC:
        if (b == BRIDGE_SYNC_BYTE) {
            s_state = ST_TYPE;
        }
        break;
    case ST_TYPE:
        s_msg_type = b;
        s_checksum = b;
        s_state = ST_ADDR;
        break;
    case ST_ADDR:
        s_dev_addr = b;
        s_checksum ^= b;
        s_state = ST_IDX;
        break;
    case ST_IDX:
        s_idx = b;
        s_checksum ^= b;
        s_state = ST_PROTO;
        break;
    case ST_PROTO:
        s_itf_protocol = b;
        s_checksum ^= b;
        s_state = ST_LEN_LO;
        break;
    case ST_LEN_LO:
        s_len = b;
        s_checksum ^= b;
        s_state = ST_LEN_HI;
        break;
    case ST_LEN_HI:
        s_len |= (uint16_t)((uint16_t)b << 8);
        s_checksum ^= b;
        if (s_len > MAX_PAYLOAD_LEN) {
            ESP_LOGW(TAG, "frame len %u exceeds max %u, resyncing", (unsigned)s_len, (unsigned)MAX_PAYLOAD_LEN);
            reset_parser();
            break;
        }
        s_payload_idx = 0;
        s_state = (s_len == 0) ? ST_CHECKSUM : ST_PAYLOAD;
        break;
    case ST_PAYLOAD:
        s_payload[s_payload_idx++] = b;
        s_checksum ^= b;
        if (s_payload_idx >= s_len) {
            s_state = ST_CHECKSUM;
        }
        break;
    case ST_CHECKSUM:
        if (b == s_checksum) {
            handle_frame();
        } else {
#if BRIDGE_RATE_MONITOR
            s_checksum_fail_count++;
#endif
            ESP_LOGW(TAG, "checksum mismatch (msg_type=0x%02x), resyncing", s_msg_type);
        }
        reset_parser();
        break;
    }
}

#if BRIDGE_RATE_MONITOR
#define MAX_RUNTIME_TASKS 24
static TaskStatus_t s_prev_task_status[MAX_RUNTIME_TASKS];
static UBaseType_t  s_prev_task_count;

// vTaskGetRunTimeStats() (tried first) reports cumulative run time
// since boot - a single ~50-80ms stall is under 1% of many seconds of
// uptime, indistinguishable from noise against tasks like IDLE0/IDLE1
// that dominate the cumulative total simply by existing the whole time
// (mds/usb_hid/2026-08-24_rp2040_bridge_fps_investigation.md's first attempt
// showed exactly this: IDLE0/IDLE1 at 93-95%, everything else <1%,
// nothing pointing at a culprit). Snapshotting uxTaskGetSystemState()
// every window and diffing against the previous snapshot instead makes
// an 80ms stall obviously visible - 8% of a ~1s window - against
// whichever task actually consumed it that window.
static void snapshot_task_runtime(bool print, uint32_t loop_gap_us)
{
    static TaskStatus_t status[MAX_RUNTIME_TASKS];
    uint32_t total_runtime;
    UBaseType_t count = uxTaskGetSystemState(status, MAX_RUNTIME_TASKS, &total_runtime);

    if (print) {
        ESP_LOGW(TAG, "[rate] loop gap %uus - per-task CPU delta over last ~1s:", (unsigned)loop_gap_us);
        for (UBaseType_t i = 0; i < count; i++) {
            uint32_t prev = 0;
            for (UBaseType_t j = 0; j < s_prev_task_count; j++) {
                if (s_prev_task_status[j].xHandle == status[i].xHandle) {
                    prev = s_prev_task_status[j].ulRunTimeCounter;
                    break;
                }
            }
            uint32_t delta = status[i].ulRunTimeCounter - prev;
            if (delta > 0) {
                ESP_LOGW(TAG, "  %-16s %uus", status[i].pcTaskName, (unsigned)delta);
            }
        }
    }

    memcpy(s_prev_task_status, status, sizeof(TaskStatus_t) * count);
    s_prev_task_count = count;
}
#endif

static void bridge_task(void *arg)
{
    (void)arg;
    uint8_t buf[64];
#if BRIDGE_RATE_MONITOR
    TickType_t last_print = xTaskGetTickCount();
    // RP2040 itself (rp2040_host_bridge.ino's own RATE_MONITOR, checked
    // directly over its Serial2 debug port) measured a rock-solid
    // ~10000us +-6us report cadence with zero bursting -
    // mds/usb_hid/2026-08-24_rp2040_bridge_fps_investigation.md follow-up. That
    // clears the RP2040/mouse/dongle entirely, meaning the ~12-50us min
    // / ~50ms max interval spread measured *here* has to come from this
    // task not getting scheduled promptly, not from data arriving late.
    // Tracks the longest gap between the start of two consecutive
    // while(1) iterations - each iteration should normally take well
    // under uart_read_bytes()'s own 20ms timeout, since new bytes are
    // arriving every ~10ms; a gap far beyond that is this task sitting
    // ready-to-run but not actually getting CPU time, i.e. direct
    // evidence of external starvation (candidates: WiFi/lwIP internal
    // tasks, USB ISR load, something else) rather than anything wrong
    // with this task's own logic.
    int64_t  last_loop_us = esp_timer_get_time();
    uint32_t max_loop_gap_us = 0;
#endif
    while (1) {
#if BRIDGE_RATE_MONITOR
        int64_t loop_now_us = esp_timer_get_time();
        uint32_t loop_gap = (uint32_t)(loop_now_us - last_loop_us);
        if (loop_gap > max_loop_gap_us) {
            max_loop_gap_us = loop_gap;
        }
        last_loop_us = loop_now_us;
#endif
        // Temporarily 1ms (was 20) - diagnostic test
        // (mds/usb_hid/2026-08-24_rp2040_bridge_fps_investigation.md): with
        // WiFi/type-c/dispatch_task all removed and the CPU otherwise
        // ~99% idle, bridge_task still saw ~50ms loop gaps. If shrinking
        // this timeout shrinks the observed gap proportionally, the
        // stall is tied to this call's own timeout handling; if the gap
        // stays ~50ms regardless, it's unrelated to this parameter -
        // something lower-level (interrupt servicing, invisible to any
        // task-level runtime stats) is the real cause.
        int n = uart_read_bytes(BRIDGE_UART_PORT, buf, sizeof(buf), pdMS_TO_TICKS(1));
        for (int i = 0; i < n; i++) {
            feed_byte(buf[i]);
        }
#if BRIDGE_RATE_MONITOR
        TickType_t now = xTaskGetTickCount();
        if (now - last_print >= pdMS_TO_TICKS(1000)) {
            last_print = now;
            uint32_t reports = s_report_count;
            uint32_t fails = s_checksum_fail_count;
            uint32_t drops = s_queue_drop_count;
            uint32_t min_interval = s_min_interval_us;
            uint32_t max_interval = s_max_interval_us;
            uint32_t loop_gap_print = max_loop_gap_us;
            s_report_count = 0;
            s_checksum_fail_count = 0;
            s_queue_drop_count = 0;
            s_min_interval_us = 0;
            s_max_interval_us = 0;
            max_loop_gap_us = 0;
            ESP_LOGI(TAG, "[rate] %u reports/sec, %u checksum failures/sec, %u queue drops/sec, min interval %uus, max interval %uus, max loop gap %uus",
                     (unsigned)reports, (unsigned)fails, (unsigned)drops, (unsigned)min_interval, (unsigned)max_interval,
                     (unsigned)loop_gap_print);

            // A loop gap far beyond uart_read_bytes()'s own 20ms
            // timeout means something else held the CPU long enough to
            // starve this (priority 6) task - see which task actually
            // consumed that time (snapshot every window regardless, to
            // keep the delta baseline current; only print detail on the
            // windows where something anomalous happened).
            snapshot_task_runtime(loop_gap_print > 5000, loop_gap_print);
        }
#endif
    }
}

// Runs dispatch_mount()/dispatch_umount()/dispatch_report() - the actual
// (possibly-blocking, see the bridge_frame_t comment above) forwarding
// work - on its own task so it can never stall bridge_task()'s UART
// draining above.
static void dispatch_task(void *arg)
{
    (void)arg;
    bridge_frame_t frame;
    while (1) {
        if (xQueueReceive(s_frame_queue, &frame, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (frame.msg_type) {
        case BRIDGE_MSG_MOUNT:
            dispatch_mount(frame.dev_addr, frame.idx, frame.itf_protocol, frame.payload, frame.len);
            break;
        case BRIDGE_MSG_UNMOUNT:
            dispatch_umount(frame.dev_addr, frame.idx);
            break;
        case BRIDGE_MSG_REPORT:
            dispatch_report(frame.dev_addr, frame.idx, frame.itf_protocol, frame.payload, frame.len);
            break;
        default:
            break;
        }
    }
}

bool usb_host_rp2040_bridge_probe(void)
{
    if (bridge_uart_init() != ESP_OK) {
        ESP_LOGI(TAG, "RP2040 bridge probe: UART init failed - assuming not present");
        return false;
    }

    s_frame_seen = false;
    reset_parser();

    // The RP2040 side sends a heartbeat frame periodically regardless of
    // USB device state, so a single validated (checksummed) frame within
    // a generous window is strong evidence of a real bridge, not line
    // noise - unlike MAX3421's single-byte revision check, a multi-byte
    // frame passing a checksum by chance is astronomically unlikely.
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(800);
    uint8_t buf[32];
    while (xTaskGetTickCount() < deadline) {
        int n = uart_read_bytes(BRIDGE_UART_PORT, buf, sizeof(buf), pdMS_TO_TICKS(50));
        for (int i = 0; i < n; i++) {
            feed_byte(buf[i]);
        }
        if (s_frame_seen) {
            ESP_LOGI(TAG, "RP2040 bridge probe: got a validated frame - present");
            return true;
        }
    }
    ESP_LOGI(TAG, "RP2040 bridge probe: no validated frame within timeout - not present");
    return false;
}

esp_err_t usb_host_rp2040_bridge_task_start(void)
{
    // bridge_task (UART parsing) is a higher priority than dispatch_task
    // (the actual, possibly-blocking forwarding work) so a stalled
    // dispatch_task can never delay bridge_task from draining the UART
    // RX buffer - see the bridge_frame_t comment above for why this
    // split exists.
    if (xTaskCreate(bridge_task, "usb_host_rp2040br", 4096, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
#if !BRIDGE_MINIMAL_TEST
    if (xTaskCreate(dispatch_task, "usb_host_rp2040disp", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
#endif
    return ESP_OK;
}
