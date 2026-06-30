#pragma once

#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

typedef struct {
    gpio_num_t pin;
    const char *name;
    uint8_t low_key;
    uint8_t high_key;
    uint16_t low_press;
    uint16_t low_release;
    uint16_t high_press;
    uint16_t high_release;
} solar_os_joystick_axis_def_t;

esp_err_t solar_os_joystick_init(void);
size_t solar_os_joystick_read_chars(char *buffer, size_t buffer_len);
