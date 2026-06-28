#include "solar_os_tui.h"

#include <stdbool.h>
#include <stddef.h>

#include "solar_os_terminal.h"

#define TUI_BOX_H 0x2500U
#define TUI_BOX_V 0x2502U
#define TUI_BOX_TL 0x250cU
#define TUI_BOX_TR 0x2510U
#define TUI_BOX_BL 0x2514U
#define TUI_BOX_BR 0x2518U

static bool tui_valid(const solar_os_tui_t *tui)
{
    return tui != NULL && tui->terminal != NULL;
}

static void tui_set_attr(solar_os_terminal_t *term, uint8_t attr)
{
    solar_os_terminal_set_bold(term, (attr & SOLAR_OS_TUI_ATTR_BOLD) != 0);
    solar_os_terminal_set_italic(term, (attr & SOLAR_OS_TUI_ATTR_ITALIC) != 0);
    solar_os_terminal_set_underline(term, (attr & SOLAR_OS_TUI_ATTR_UNDERLINE) != 0);
    solar_os_terminal_set_inverse(term, (attr & SOLAR_OS_TUI_ATTR_INVERSE) != 0);
}

static void tui_restore_attr(solar_os_terminal_t *term,
                             bool bold,
                             bool italic,
                             bool underline,
                             bool inverse)
{
    solar_os_terminal_set_bold(term, bold);
    solar_os_terminal_set_italic(term, italic);
    solar_os_terminal_set_underline(term, underline);
    solar_os_terminal_set_inverse(term, inverse);
}

static esp_err_t tui_validate_origin(const solar_os_tui_t *tui, size_t row, size_t col)
{
    if (!tui_valid(tui)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (row >= solar_os_terminal_rows(tui->terminal) ||
        col >= solar_os_terminal_cols(tui->terminal)) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t solar_os_tui_begin(solar_os_tui_t *tui, solar_os_context_t *ctx)
{
    if (tui == NULL || ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    tui->terminal = solar_os_context_terminal(ctx);
    return tui->terminal != NULL ? ESP_OK : ESP_ERR_INVALID_STATE;
}

size_t solar_os_tui_rows(const solar_os_tui_t *tui)
{
    return tui_valid(tui) ? solar_os_terminal_rows(tui->terminal) : 0;
}

size_t solar_os_tui_cols(const solar_os_tui_t *tui)
{
    return tui_valid(tui) ? solar_os_terminal_cols(tui->terminal) : 0;
}

void solar_os_tui_clear(solar_os_tui_t *tui)
{
    if (tui_valid(tui)) {
        solar_os_terminal_clear(tui->terminal);
    }
}

void solar_os_tui_refresh(solar_os_tui_t *tui)
{
    if (tui_valid(tui)) {
        solar_os_terminal_draw(tui->terminal);
    }
}

esp_err_t solar_os_tui_move(solar_os_tui_t *tui, size_t row, size_t col)
{
    esp_err_t err = tui_validate_origin(tui, row, col);
    if (err != ESP_OK) {
        return err;
    }

    solar_os_terminal_set_cursor(tui->terminal, row, col);
    return ESP_OK;
}

esp_err_t solar_os_tui_write(solar_os_tui_t *tui, const char *text, uint8_t attr)
{
    if (!tui_valid(tui) || text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const bool bold = solar_os_terminal_bold(tui->terminal);
    const bool italic = solar_os_terminal_italic(tui->terminal);
    const bool underline = solar_os_terminal_underline(tui->terminal);
    const bool inverse = solar_os_terminal_inverse(tui->terminal);
    tui_set_attr(tui->terminal, attr);
    solar_os_terminal_write_utf8(tui->terminal, text);
    tui_restore_attr(tui->terminal, bold, italic, underline, inverse);
    return ESP_OK;
}

esp_err_t solar_os_tui_addstr(solar_os_tui_t *tui,
                              size_t row,
                              size_t col,
                              const char *text,
                              uint8_t attr)
{
    esp_err_t err = solar_os_tui_move(tui, row, col);
    if (err != ESP_OK) {
        return err;
    }
    return solar_os_tui_write(tui, text, attr);
}

esp_err_t solar_os_tui_putch(solar_os_tui_t *tui,
                             size_t row,
                             size_t col,
                             uint32_t codepoint,
                             uint8_t attr)
{
    esp_err_t err = solar_os_tui_move(tui, row, col);
    if (err != ESP_OK) {
        return err;
    }

    const bool bold = solar_os_terminal_bold(tui->terminal);
    const bool italic = solar_os_terminal_italic(tui->terminal);
    const bool underline = solar_os_terminal_underline(tui->terminal);
    const bool inverse = solar_os_terminal_inverse(tui->terminal);
    tui_set_attr(tui->terminal, attr);
    solar_os_terminal_put_codepoint(tui->terminal, codepoint);
    tui_restore_attr(tui->terminal, bold, italic, underline, inverse);
    return ESP_OK;
}

esp_err_t solar_os_tui_hline(solar_os_tui_t *tui,
                             size_t row,
                             size_t col,
                             size_t width,
                             uint32_t codepoint,
                             uint8_t attr)
{
    if (width == 0) {
        return ESP_OK;
    }
    esp_err_t err = tui_validate_origin(tui, row, col);
    if (err != ESP_OK) {
        return err;
    }

    const size_t cols = solar_os_terminal_cols(tui->terminal);
    const size_t draw_width = col + width > cols ? cols - col : width;
    const uint32_t glyph = codepoint != 0 ? codepoint : TUI_BOX_H;

    const bool bold = solar_os_terminal_bold(tui->terminal);
    const bool italic = solar_os_terminal_italic(tui->terminal);
    const bool underline = solar_os_terminal_underline(tui->terminal);
    const bool inverse = solar_os_terminal_inverse(tui->terminal);
    tui_set_attr(tui->terminal, attr);
    solar_os_terminal_set_cursor(tui->terminal, row, col);
    for (size_t i = 0; i < draw_width; i++) {
        solar_os_terminal_put_codepoint(tui->terminal, glyph);
    }
    tui_restore_attr(tui->terminal, bold, italic, underline, inverse);
    return ESP_OK;
}

esp_err_t solar_os_tui_vline(solar_os_tui_t *tui,
                             size_t row,
                             size_t col,
                             size_t height,
                             uint32_t codepoint,
                             uint8_t attr)
{
    if (height == 0) {
        return ESP_OK;
    }
    esp_err_t err = tui_validate_origin(tui, row, col);
    if (err != ESP_OK) {
        return err;
    }

    const size_t rows = solar_os_terminal_rows(tui->terminal);
    const size_t draw_height = row + height > rows ? rows - row : height;
    const uint32_t glyph = codepoint != 0 ? codepoint : TUI_BOX_V;

    const bool bold = solar_os_terminal_bold(tui->terminal);
    const bool italic = solar_os_terminal_italic(tui->terminal);
    const bool underline = solar_os_terminal_underline(tui->terminal);
    const bool inverse = solar_os_terminal_inverse(tui->terminal);
    tui_set_attr(tui->terminal, attr);
    for (size_t i = 0; i < draw_height; i++) {
        solar_os_terminal_set_cursor(tui->terminal, row + i, col);
        solar_os_terminal_put_codepoint(tui->terminal, glyph);
    }
    tui_restore_attr(tui->terminal, bold, italic, underline, inverse);
    return ESP_OK;
}

esp_err_t solar_os_tui_vrule(solar_os_tui_t *tui,
                             size_t row,
                             size_t col,
                             size_t height,
                             uint8_t width,
                             uint8_t attr)
{
    if (height == 0) {
        return ESP_OK;
    }
    esp_err_t err = tui_validate_origin(tui, row, col);
    if (err != ESP_OK) {
        return err;
    }

    return solar_os_terminal_add_vrule(tui->terminal,
                                       row,
                                       col,
                                       height,
                                       width,
                                       (attr & SOLAR_OS_TUI_ATTR_INVERSE) != 0);
}

esp_err_t solar_os_tui_box(solar_os_tui_t *tui,
                           size_t row,
                           size_t col,
                           size_t height,
                           size_t width,
                           uint8_t attr)
{
    if (height == 0 || width == 0) {
        return ESP_OK;
    }
    esp_err_t err = tui_validate_origin(tui, row, col);
    if (err != ESP_OK) {
        return err;
    }

    const size_t rows = solar_os_terminal_rows(tui->terminal);
    const size_t cols = solar_os_terminal_cols(tui->terminal);
    const size_t clipped_height = row + height > rows ? rows - row : height;
    const size_t clipped_width = col + width > cols ? cols - col : width;

    if (clipped_height == 1) {
        return solar_os_tui_hline(tui, row, col, clipped_width, TUI_BOX_H, attr);
    }
    if (clipped_width == 1) {
        return solar_os_tui_vline(tui, row, col, clipped_height, TUI_BOX_V, attr);
    }

    solar_os_tui_putch(tui, row, col, TUI_BOX_TL, attr);
    solar_os_tui_putch(tui, row, col + clipped_width - 1, TUI_BOX_TR, attr);
    solar_os_tui_putch(tui, row + clipped_height - 1, col, TUI_BOX_BL, attr);
    solar_os_tui_putch(tui,
                       row + clipped_height - 1,
                       col + clipped_width - 1,
                       TUI_BOX_BR,
                       attr);
    solar_os_tui_hline(tui, row, col + 1, clipped_width - 2, TUI_BOX_H, attr);
    solar_os_tui_hline(tui,
                       row + clipped_height - 1,
                       col + 1,
                       clipped_width - 2,
                       TUI_BOX_H,
                       attr);
    solar_os_tui_vline(tui, row + 1, col, clipped_height - 2, TUI_BOX_V, attr);
    solar_os_tui_vline(tui,
                       row + 1,
                       col + clipped_width - 1,
                       clipped_height - 2,
                       TUI_BOX_V,
                       attr);
    return ESP_OK;
}

esp_err_t solar_os_tui_fill(solar_os_tui_t *tui,
                            size_t row,
                            size_t col,
                            size_t height,
                            size_t width,
                            uint32_t codepoint,
                            uint8_t attr)
{
    if (height == 0 || width == 0) {
        return ESP_OK;
    }
    esp_err_t err = tui_validate_origin(tui, row, col);
    if (err != ESP_OK) {
        return err;
    }

    const size_t rows = solar_os_terminal_rows(tui->terminal);
    const size_t cols = solar_os_terminal_cols(tui->terminal);
    const size_t draw_height = row + height > rows ? rows - row : height;
    const size_t draw_width = col + width > cols ? cols - col : width;
    const uint32_t glyph = codepoint != 0 ? codepoint : ' ';

    const bool bold = solar_os_terminal_bold(tui->terminal);
    const bool italic = solar_os_terminal_italic(tui->terminal);
    const bool underline = solar_os_terminal_underline(tui->terminal);
    const bool inverse = solar_os_terminal_inverse(tui->terminal);
    tui_set_attr(tui->terminal, attr);
    for (size_t y = 0; y < draw_height; y++) {
        solar_os_terminal_set_cursor(tui->terminal, row + y, col);
        for (size_t x = 0; x < draw_width; x++) {
            solar_os_terminal_put_codepoint(tui->terminal, glyph);
        }
    }
    tui_restore_attr(tui->terminal, bold, italic, underline, inverse);
    return ESP_OK;
}
