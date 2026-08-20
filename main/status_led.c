#include "status_led.h"

#include "driver/gpio.h"

// XIAO ESP32S3 onboard user LED: GPIO21, active-low (LOW = on).
#define STATUS_LED_GPIO GPIO_NUM_21

void status_led_init(void)
{
    gpio_reset_pin(STATUS_LED_GPIO);
    gpio_set_direction(STATUS_LED_GPIO, GPIO_MODE_OUTPUT);
    status_led_set(false);
}

void status_led_set(bool on)
{
    gpio_set_level(STATUS_LED_GPIO, on ? 0 : 1);
}
