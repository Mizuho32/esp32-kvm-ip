#include "status_led.h"

#include <stddef.h>

#include "driver/gpio.h"
#include "esp_timer.h"

// XIAO ESP32S3 onboard user LED: GPIO21, active-low (LOW = on).
#define STATUS_LED_GPIO GPIO_NUM_21

// WiFi "connecting" pattern: a steady metronome toggle - deliberately
// simple and deliberately different-feeling from the BLE pattern below
// (mds/usb_hid/2026-09-07_ble_hid_sink_plan.md's "同じ点滅だと状態を見分け
// られない" concern).
#define BLINK_INTERVAL_US (250 * 1000)

static esp_timer_handle_t s_blink_timer;
static bool s_blink_level;

// BLE "advertising" pattern: two quick blinks then a pause (1s cycle) -
// reads as a distinct "double pulse" next to the WiFi pattern's even
// toggle. Driven by a self-rescheduling one-shot timer (each step's
// duration differs, so a plain esp_timer_start_periodic() can't express
// it the way the WiFi pattern's symmetric toggle can).
static const struct {
    bool level;
    uint32_t duration_us;
} s_ble_adv_pattern[] = {
    { true,  100 * 1000 },
    { false, 100 * 1000 },
    { true,  100 * 1000 },
    { false, 700 * 1000 },
};
#define BLE_ADV_PATTERN_LEN (sizeof(s_ble_adv_pattern) / sizeof(s_ble_adv_pattern[0]))

static esp_timer_handle_t s_ble_timer;
static size_t s_ble_step;

// Desired state, set only by the three public setters below - refresh()
// is the single place that turns these into an actual GPIO level/timer,
// applying mds/usb_hid/2026-09-07_ble_hid_sink_plan.md's priority rule.
static bool s_wifi_connecting;
static bool s_ble_advertising;
static bool s_solid_level;

static void set_level(bool on)
{
    gpio_set_level(STATUS_LED_GPIO, on ? 0 : 1);
}

static void blink_timer_cb(void *arg)
{
    (void)arg;
    s_blink_level = !s_blink_level;
    set_level(s_blink_level);
}

static void ble_pattern_timer_cb(void *arg)
{
    (void)arg;
    set_level(s_ble_adv_pattern[s_ble_step].level);
    esp_timer_start_once(s_ble_timer, s_ble_adv_pattern[s_ble_step].duration_us);
    s_ble_step = (s_ble_step + 1) % BLE_ADV_PATTERN_LEN;
}

// Re-derives the LED's actual level/timer from the three desired-state
// flags above, applying priority: WiFi's connecting-blink first, then
// solid-off (WiFi not connected/USB-suspended - BLE's own pattern never
// shows through that), then BLE's advertising pattern, then solid-on.
// See status_led.h's doc comments for the reasoning.
static void refresh(void)
{
    if (s_wifi_connecting) {
        esp_timer_stop(s_ble_timer); // no-op (ignored error) if not running
        if (!esp_timer_is_active(s_blink_timer)) {
            esp_timer_start_periodic(s_blink_timer, BLINK_INTERVAL_US);
        }
    } else if (!s_solid_level) {
        esp_timer_stop(s_blink_timer);
        esp_timer_stop(s_ble_timer);
        set_level(false);
    } else if (s_ble_advertising) {
        esp_timer_stop(s_blink_timer);
        if (!esp_timer_is_active(s_ble_timer)) {
            s_ble_step = 0;
            ble_pattern_timer_cb(NULL); // shows step 0 immediately, arms the rest
        }
    } else {
        esp_timer_stop(s_blink_timer);
        esp_timer_stop(s_ble_timer);
        set_level(true);
    }
}

void status_led_init(void)
{
    gpio_reset_pin(STATUS_LED_GPIO);
    gpio_set_direction(STATUS_LED_GPIO, GPIO_MODE_OUTPUT);
    set_level(false);

    const esp_timer_create_args_t blink_timer_args = {
        .callback = blink_timer_cb,
        .name = "status_led_blink",
    };
    esp_timer_create(&blink_timer_args, &s_blink_timer);

    const esp_timer_create_args_t ble_timer_args = {
        .callback = ble_pattern_timer_cb,
        .name = "status_led_ble",
    };
    esp_timer_create(&ble_timer_args, &s_ble_timer);
}

void status_led_set(bool on)
{
    s_solid_level = on;
    s_wifi_connecting = false;
    refresh();
}

void status_led_set_blinking(bool blinking)
{
    s_wifi_connecting = blinking;
    refresh();
}

void status_led_set_ble_advertising(bool advertising)
{
    s_ble_advertising = advertising;
    refresh();
}
