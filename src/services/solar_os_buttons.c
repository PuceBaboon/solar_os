#include "solar_os_buttons.h"

#include "esp_timer.h"
#include "solar_os_board_caps.h"
#include "solar_os_log.h"

#if SOLAR_OS_BOARD_HAS_BUTTONS
#include "solar_os_board.h"

#ifndef SOLAR_OS_BOARD_BUTTONS
#error "Board enables BUTTONS but does not define SOLAR_OS_BOARD_BUTTONS."
#endif

#define BUTTON_DEBOUNCE_MS 25U

typedef struct {
    bool initialized;
    bool last_raw_pressed;
    bool stable_pressed;
    uint32_t raw_changed_ms;
} button_state_t;

static const char *TAG = "solar_os_buttons";
static const solar_os_button_def_t button_defs[] = SOLAR_OS_BOARD_BUTTONS;
static button_state_t button_states[sizeof(button_defs) / sizeof(button_defs[0])];
static bool buttons_initialized;

static uint32_t buttons_millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static gpio_pullup_t button_pull_up(solar_os_button_pull_t pull)
{
    return pull == SOLAR_OS_BUTTON_PULL_UP ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
}

static gpio_pulldown_t button_pull_down(solar_os_button_pull_t pull)
{
    return pull == SOLAR_OS_BUTTON_PULL_DOWN ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
}

static bool button_raw_pressed(const solar_os_button_def_t *def)
{
    const int level = gpio_get_level(def->pin);
    return def->active_low ? level == 0 : level != 0;
}
#endif

esp_err_t solar_os_buttons_init(void)
{
#if !SOLAR_OS_BOARD_HAS_BUTTONS
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (buttons_initialized) {
        return ESP_OK;
    }

    for (size_t i = 0; i < sizeof(button_defs) / sizeof(button_defs[0]); i++) {
        const solar_os_button_def_t *def = &button_defs[i];
        if (!GPIO_IS_VALID_GPIO(def->pin)) {
            return ESP_ERR_INVALID_ARG;
        }

        const gpio_config_t config = {
            .pin_bit_mask = 1ULL << (uint32_t)def->pin,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = button_pull_up(def->pull),
            .pull_down_en = button_pull_down(def->pull),
            .intr_type = GPIO_INTR_DISABLE,
        };
        const esp_err_t ret = gpio_config(&config);
        if (ret != ESP_OK) {
            return ret;
        }

        const bool pressed = button_raw_pressed(def);
        button_states[i] = (button_state_t) {
            .initialized = true,
            .last_raw_pressed = pressed,
            .stable_pressed = pressed,
            .raw_changed_ms = buttons_millis(),
        };
    }

    buttons_initialized = true;
    SOLAR_OS_LOGI(TAG, "%u board buttons ready", (unsigned)(sizeof(button_defs) / sizeof(button_defs[0])));
    return ESP_OK;
#endif
}

size_t solar_os_buttons_read_chars(char *buffer, size_t buffer_len)
{
#if !SOLAR_OS_BOARD_HAS_BUTTONS
    (void)buffer;
    (void)buffer_len;
    return 0;
#else
    if (buffer == NULL || buffer_len == 0 || !buttons_initialized) {
        return 0;
    }

    const uint32_t now_ms = buttons_millis();
    size_t count = 0;

    for (size_t i = 0; i < sizeof(button_defs) / sizeof(button_defs[0]); i++) {
        const solar_os_button_def_t *def = &button_defs[i];
        button_state_t *state = &button_states[i];
        const bool pressed = button_raw_pressed(def);

        if (!state->initialized) {
            *state = (button_state_t) {
                .initialized = true,
                .last_raw_pressed = pressed,
                .stable_pressed = pressed,
                .raw_changed_ms = now_ms,
            };
            continue;
        }

        if (pressed != state->last_raw_pressed) {
            state->last_raw_pressed = pressed;
            state->raw_changed_ms = now_ms;
            continue;
        }

        if (pressed == state->stable_pressed ||
            (uint32_t)(now_ms - state->raw_changed_ms) < BUTTON_DEBOUNCE_MS) {
            continue;
        }

        state->stable_pressed = pressed;
        if (!pressed || def->key == 0) {
            continue;
        }

        if (count >= buffer_len) {
            break;
        }
        buffer[count++] = (char)def->key;
    }

    return count;
#endif
}
