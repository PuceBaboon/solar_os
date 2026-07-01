#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct solar_os_board_display solar_os_board_display_t;

esp_err_t solar_os_display_init(solar_os_board_display_t *display);
bool solar_os_display_brightness_supported(void);
esp_err_t solar_os_display_get_brightness(uint8_t *percent);
esp_err_t solar_os_display_set_brightness(uint8_t percent);
