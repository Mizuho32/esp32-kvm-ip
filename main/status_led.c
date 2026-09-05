#include "status_led.h"

#include "driver/gpio.h"
#include "esp_timer.h"

// XIAO ESP32S3 onboard user LED: GPIO21, active-low (LOW = on).
#define STATUS_LED_GPIO GPIO_NUM_21

#define BLINK_INTERVAL_US (250 * 1000)

static esp_timer_handle_t s_blink_timer;
static bool s_blink_level;

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

void status_led_init(void)
{
    gpio_reset_pin(STATUS_LED_GPIO);
    gpio_set_direction(STATUS_LED_GPIO, GPIO_MODE_OUTPUT);
    set_level(false);

    const esp_timer_create_args_t timer_args = {
        .callback = blink_timer_cb,
        .name = "status_led_blink",
    };
    esp_timer_create(&timer_args, &s_blink_timer);
}

void status_led_set(bool on)
{
    if (s_blink_timer != NULL) {
        esp_timer_stop(s_blink_timer); // no-op (returns an ignored error) if not running
    }
    set_level(on);
}

void status_led_set_blinking(bool blinking)
{
    if (s_blink_timer == NULL) {
        return;
    }
    if (blinking) {
        esp_timer_start_periodic(s_blink_timer, BLINK_INTERVAL_US);
    } else {
        esp_timer_stop(s_blink_timer);
    }
}
