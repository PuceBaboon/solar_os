#include "solar_os_display.h"

#include "solar_os_board_caps.h"

#if SOLAR_OS_BOARD_HAS_DISPLAY
#include "nvs.h"
#include "solar_os_board_display.h"
#endif

#define DISPLAY_NVS_NAMESPACE "display"
#define DISPLAY_NVS_BRIGHTNESS_KEY "brightness"
#define DISPLAY_DEFAULT_BRIGHTNESS 100U

#if SOLAR_OS_BOARD_HAS_DISPLAY
static solar_os_board_display_t *display_handle;
static uint8_t display_brightness = DISPLAY_DEFAULT_BRIGHTNESS;

static esp_err_t display_save_brightness(uint8_t percent)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(DISPLAY_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_u8(nvs, DISPLAY_NVS_BRIGHTNESS_KEY, percent);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

static uint8_t display_load_brightness(void)
{
    nvs_handle_t nvs;
    uint8_t percent = DISPLAY_DEFAULT_BRIGHTNESS;

    if (nvs_open(DISPLAY_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return percent;
    }

    uint8_t stored = DISPLAY_DEFAULT_BRIGHTNESS;
    if (nvs_get_u8(nvs, DISPLAY_NVS_BRIGHTNESS_KEY, &stored) == ESP_OK && stored <= 100) {
        percent = stored;
    }
    nvs_close(nvs);
    return percent;
}
#endif

esp_err_t solar_os_display_init(solar_os_board_display_t *display)
{
#if !SOLAR_OS_BOARD_HAS_DISPLAY
    (void)display;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (display == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    display_handle = display;
    display_brightness = display_load_brightness();
    const esp_err_t err = solar_os_board_display_set_brightness(display_handle, display_brightness);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return ESP_OK;
    }
    return err;
#endif
}

bool solar_os_display_brightness_supported(void)
{
#if !SOLAR_OS_BOARD_HAS_DISPLAY || !SOLAR_OS_BOARD_HAS_DISPLAY_BRIGHTNESS
    return false;
#else
    return display_handle != NULL &&
        solar_os_board_display_brightness_supported(display_handle);
#endif
}

esp_err_t solar_os_display_get_brightness(uint8_t *percent)
{
    if (percent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
#if !SOLAR_OS_BOARD_HAS_DISPLAY
    *percent = 0;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (display_handle == NULL) {
        *percent = 0;
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = solar_os_board_display_get_brightness(display_handle, percent);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        *percent = display_brightness;
    }
    return err;
#endif
}

esp_err_t solar_os_display_set_brightness(uint8_t percent)
{
    if (percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
#if !SOLAR_OS_BOARD_HAS_DISPLAY
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (display_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = solar_os_board_display_set_brightness(display_handle, percent);
    if (ret != ESP_OK) {
        return ret;
    }

    display_brightness = percent;
    ret = display_save_brightness(percent);
    return ret;
#endif
}

