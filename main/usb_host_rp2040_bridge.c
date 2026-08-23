#include "usb_host_rp2040_bridge.h"

#include <string.h>

#include "esp_log.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "class/hid/hid.h" // hid_keyboard_report_t / hid_mouse_report_t only - no tuh_*/tud_* dependency

#include "hid_forwarder.h"
#include "hid_report_parser.h"

#define TAG "USBHOST_RP2040BRIDGE"

// Placeholder pins - adjust to match actual wiring, same as
// usb_host_max3421.c's MAX3421_PIN_*. Deliberately different from the
// MAX3421 SPI/GPIO pins (4,5,6,7,8,9) and the debug UART0 (43,44) so
// both can be wired at once if ever needed (only one backend actually
// runs at a time - see main_host.c).
#define BRIDGE_UART_PORT  UART_NUM_1
#define BRIDGE_UART_TX_PIN 17
#define BRIDGE_UART_RX_PIN 18
#define BRIDGE_UART_BAUD  460800

// Frame format (see mds/2026-08-23_rp2040_as_host_bridge_plan.md):
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
} bridge_msg_type_t;

// USB HID spec bInterfaceProtocol values (not a TinyUSB-specific enum -
// this file deliberately doesn't include any tuh_*/tud_* header, only
// class/hid/hid.h for the two Boot Protocol report structs). The RP2040
// side sends these verbatim from its own tuh_hid_interface_protocol().
#define ITF_PROTOCOL_NONE     0
#define ITF_PROTOCOL_KEYBOARD 1
#define ITF_PROTOCOL_MOUSE    2

#define MAX_PAYLOAD_LEN 512

static bool s_uart_initialized;

static esp_err_t bridge_uart_init(void)
{
    if (s_uart_initialized) {
        return ESP_OK;
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
    ESP_LOGI(TAG, "HID mounted: dev_addr=%d idx=%d itf_protocol=%d, report descriptor (%d bytes):",
             dev_addr, idx, itf_protocol, (int)desc_len);
    ESP_LOG_BUFFER_HEX(TAG, report_desc, desc_len);

    if (itf_protocol == ITF_PROTOCOL_MOUSE) {
        bridge_mouse_state_t *dev = register_mouse_device(dev_addr, idx);
        if (dev && report_desc && desc_len > 0) {
            hid_parse_mouse_report_descriptor(report_desc, desc_len, &dev->layout);
            dev->use_report_protocol = dev->layout.x.present && dev->layout.y.present;

            hid_parse_consumer_report_descriptor(report_desc, desc_len, &dev->consumer_layout);
            if (dev->consumer_layout.selector.present) {
                ESP_LOGI(TAG, "Mouse also has a bundled Consumer Control selector (report_id=%d bit_length=%d)",
                         dev->consumer_layout.selector.report_id, dev->consumer_layout.selector.bit_length);
            }
            ESP_LOGI(TAG, "Mouse connected (use_report_protocol=%d buttons=%d wheel=%d pan=%d)",
                     dev->use_report_protocol, dev->layout.button_count, dev->layout.wheel.present, dev->layout.pan.present);
        }
    } else if (itf_protocol == ITF_PROTOCOL_NONE) {
        consumer_report_layout_t layout = {0};
        if (report_desc && desc_len > 0) {
            hid_parse_consumer_report_descriptor(report_desc, desc_len, &layout);
        }
        if (layout.selector.present) {
            bridge_consumer_state_t *dev = register_consumer_device(dev_addr, idx);
            if (dev) {
                dev->layout = layout;
                ESP_LOGI(TAG, "Consumer Control device connected (media keys): bit_offset=%d bit_length=%d report_id=%d",
                         layout.selector.bit_offset, layout.selector.bit_length, layout.selector.report_id);
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
    case BRIDGE_MSG_MOUNT:
        dispatch_mount(s_dev_addr, s_idx, s_itf_protocol, s_payload, s_payload_idx);
        break;
    case BRIDGE_MSG_UNMOUNT:
        dispatch_umount(s_dev_addr, s_idx);
        break;
    case BRIDGE_MSG_REPORT:
        dispatch_report(s_dev_addr, s_idx, s_itf_protocol, s_payload, s_payload_idx);
        break;
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
            ESP_LOGW(TAG, "checksum mismatch (msg_type=0x%02x), resyncing", s_msg_type);
        }
        reset_parser();
        break;
    }
}

static void bridge_task(void *arg)
{
    (void)arg;
    uint8_t buf[64];
    while (1) {
        int n = uart_read_bytes(BRIDGE_UART_PORT, buf, sizeof(buf), pdMS_TO_TICKS(20));
        for (int i = 0; i < n; i++) {
            feed_byte(buf[i]);
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
    if (xTaskCreate(bridge_task, "usb_host_rp2040br", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
