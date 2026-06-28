#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os.h"

typedef enum {
    SOLAR_OS_TUI_ATTR_NORMAL = 0,
    SOLAR_OS_TUI_ATTR_BOLD = 1U << 0,
    SOLAR_OS_TUI_ATTR_INVERSE = 1U << 1,
    SOLAR_OS_TUI_ATTR_ITALIC = 1U << 2,
    SOLAR_OS_TUI_ATTR_UNDERLINE = 1U << 3,
} solar_os_tui_attr_t;

typedef struct {
    solar_os_terminal_t *terminal;
} solar_os_tui_t;

esp_err_t solar_os_tui_begin(solar_os_tui_t *tui, solar_os_context_t *ctx);
size_t solar_os_tui_rows(const solar_os_tui_t *tui);
size_t solar_os_tui_cols(const solar_os_tui_t *tui);
void solar_os_tui_clear(solar_os_tui_t *tui);
void solar_os_tui_refresh(solar_os_tui_t *tui);
esp_err_t solar_os_tui_move(solar_os_tui_t *tui, size_t row, size_t col);
esp_err_t solar_os_tui_write(solar_os_tui_t *tui, const char *text, uint8_t attr);
esp_err_t solar_os_tui_addstr(solar_os_tui_t *tui,
                              size_t row,
                              size_t col,
                              const char *text,
                              uint8_t attr);
esp_err_t solar_os_tui_putch(solar_os_tui_t *tui,
                             size_t row,
                             size_t col,
                             uint32_t codepoint,
                             uint8_t attr);
esp_err_t solar_os_tui_hline(solar_os_tui_t *tui,
                             size_t row,
                             size_t col,
                             size_t width,
                             uint32_t codepoint,
                             uint8_t attr);
esp_err_t solar_os_tui_vline(solar_os_tui_t *tui,
                             size_t row,
                             size_t col,
                             size_t height,
                             uint32_t codepoint,
                             uint8_t attr);
esp_err_t solar_os_tui_vrule(solar_os_tui_t *tui,
                             size_t row,
                             size_t col,
                             size_t height,
                             uint8_t width,
                             uint8_t attr);
esp_err_t solar_os_tui_box(solar_os_tui_t *tui,
                           size_t row,
                           size_t col,
                           size_t height,
                           size_t width,
                           uint8_t attr);
esp_err_t solar_os_tui_fill(solar_os_tui_t *tui,
                            size_t row,
                            size_t col,
                            size_t height,
                            size_t width,
                            uint32_t codepoint,
                            uint8_t attr);
