#include "solar_os_tui.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "solar_os_shell_io.h"
#include "solar_os_terminal.h"

#define TUI_BOX_H 0x2500U
#define TUI_BOX_V 0x2502U
#define TUI_BOX_TL 0x250cU
#define TUI_BOX_TR 0x2510U
#define TUI_BOX_BL 0x2514U
#define TUI_BOX_BR 0x2518U

static bool tui_valid(const solar_os_tui_t *tui)
{
    return tui != NULL && tui->io != NULL &&
        solar_os_shell_io_kind(tui->io) != SOLAR_OS_SHELL_IO_KIND_NONE;
}

static void tui_set_attr(solar_os_tui_t *tui, uint8_t attr)
{
    if (!tui_valid(tui)) {
        return;
    }

    (void)solar_os_shell_io_set_bold(tui->io, (attr & SOLAR_OS_TUI_ATTR_BOLD) != 0);
    (void)solar_os_shell_io_set_italic(tui->io, (attr & SOLAR_OS_TUI_ATTR_ITALIC) != 0);
    (void)solar_os_shell_io_set_underline(tui->io, (attr & SOLAR_OS_TUI_ATTR_UNDERLINE) != 0);
    (void)solar_os_shell_io_set_inverse(tui->io, (attr & SOLAR_OS_TUI_ATTR_INVERSE) != 0);
}

static void tui_restore_attr(solar_os_tui_t *tui,
                             bool bold,
                             bool italic,
                             bool underline,
                             bool inverse)
{
    if (!tui_valid(tui)) {
        return;
    }

    (void)solar_os_shell_io_set_bold(tui->io, bold);
    (void)solar_os_shell_io_set_italic(tui->io, italic);
    (void)solar_os_shell_io_set_underline(tui->io, underline);
    (void)solar_os_shell_io_set_inverse(tui->io, inverse);
}

static void tui_save_attr(const solar_os_tui_t *tui,
                          bool *bold,
                          bool *italic,
                          bool *underline,
                          bool *inverse)
{
    if (bold != NULL) {
        *bold = tui != NULL && tui->io != NULL && tui->io->bold;
    }
    if (italic != NULL) {
        *italic = tui != NULL && tui->io != NULL && tui->io->italic;
    }
    if (underline != NULL) {
        *underline = tui != NULL && tui->io != NULL && tui->io->underline;
    }
    if (inverse != NULL) {
        *inverse = tui != NULL && tui->io != NULL && tui->io->inverse;
    }
}

static size_t tui_rows(const solar_os_tui_t *tui)
{
    return tui_valid(tui) ? solar_os_shell_io_rows(tui->io) : 0;
}

static size_t tui_cols(const solar_os_tui_t *tui)
{
    return tui_valid(tui) ? solar_os_shell_io_cols(tui->io) : 0;
}

static esp_err_t tui_validate_origin(const solar_os_tui_t *tui, size_t row, size_t col)
{
    if (!tui_valid(tui)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (row >= tui_rows(tui) || col >= tui_cols(tui)) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static size_t tui_encode_utf8(uint32_t codepoint, char out[4])
{
    if (out == NULL) {
        return 0;
    }

    if (codepoint <= 0x7fU) {
        out[0] = (char)codepoint;
        return 1;
    }
    if (codepoint <= 0x7ffU) {
        out[0] = (char)(0xc0U | (codepoint >> 6));
        out[1] = (char)(0x80U | (codepoint & 0x3fU));
        return 2;
    }
    if (codepoint <= 0xffffU) {
        out[0] = (char)(0xe0U | (codepoint >> 12));
        out[1] = (char)(0x80U | ((codepoint >> 6) & 0x3fU));
        out[2] = (char)(0x80U | (codepoint & 0x3fU));
        return 3;
    }
    if (codepoint <= 0x10ffffU) {
        out[0] = (char)(0xf0U | (codepoint >> 18));
        out[1] = (char)(0x80U | ((codepoint >> 12) & 0x3fU));
        out[2] = (char)(0x80U | ((codepoint >> 6) & 0x3fU));
        out[3] = (char)(0x80U | (codepoint & 0x3fU));
        return 4;
    }
    out[0] = '?';
    return 1;
}

static void tui_track_cell(solar_os_tui_t *tui)
{
    if (!tui_valid(tui) || tui->io->cols == 0) {
        return;
    }

    tui->io->cursor_col++;
    if (tui->io->cursor_col >= tui->io->cols) {
        tui->io->cursor_col = 0;
        if (tui->io->rows == 0 || tui->io->cursor_row + 1U < tui->io->rows) {
            tui->io->cursor_row++;
        }
    }
}

static esp_err_t tui_write_codepoint(solar_os_tui_t *tui, uint32_t codepoint)
{
    if (!tui_valid(tui)) {
        return ESP_ERR_INVALID_STATE;
    }

    if (solar_os_shell_io_kind(tui->io) == SOLAR_OS_SHELL_IO_KIND_TERMINAL &&
        tui->terminal != NULL) {
        solar_os_terminal_put_codepoint(tui->terminal, codepoint);
        tui->io->cursor_row = solar_os_terminal_cursor_row(tui->terminal);
        tui->io->cursor_col = solar_os_terminal_cursor_col(tui->terminal);
        return ESP_OK;
    }

    char bytes[4];
    const size_t len = tui_encode_utf8(codepoint, bytes);
    const esp_err_t err = solar_os_shell_io_write_raw(tui->io, bytes, len);
    if (err == ESP_OK) {
        tui_track_cell(tui);
    }
    return err;
}

static esp_err_t tui_write_text(solar_os_tui_t *tui, const char *text)
{
    if (!tui_valid(tui) || text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (solar_os_shell_io_kind(tui->io) == SOLAR_OS_SHELL_IO_KIND_TERMINAL &&
        tui->terminal != NULL) {
        solar_os_terminal_write_utf8(tui->terminal, text);
        tui->io->cursor_row = solar_os_terminal_cursor_row(tui->terminal);
        tui->io->cursor_col = solar_os_terminal_cursor_col(tui->terminal);
        return ESP_OK;
    }

    return solar_os_shell_io_write(tui->io, text);
}

esp_err_t solar_os_tui_begin(solar_os_tui_t *tui, solar_os_context_t *ctx)
{
    if (tui == NULL || ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(tui, 0, sizeof(*tui));
    tui->io = solar_os_context_shell_io(ctx);
    if (tui->io == NULL ||
        solar_os_shell_io_kind(tui->io) == SOLAR_OS_SHELL_IO_KIND_NONE) {
        solar_os_shell_io_init_terminal(&tui->fallback_io, solar_os_context_terminal(ctx));
        tui->io = &tui->fallback_io;
    }
    tui->terminal = solar_os_shell_io_terminal(tui->io);
    return tui_valid(tui) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

size_t solar_os_tui_rows(const solar_os_tui_t *tui)
{
    return tui_rows(tui);
}

size_t solar_os_tui_cols(const solar_os_tui_t *tui)
{
    return tui_cols(tui);
}

void solar_os_tui_clear(solar_os_tui_t *tui)
{
    if (tui_valid(tui)) {
        (void)solar_os_shell_io_clear(tui->io);
    }
}

void solar_os_tui_refresh(solar_os_tui_t *tui)
{
    if (tui_valid(tui)) {
        (void)solar_os_shell_io_flush(tui->io);
    }
}

esp_err_t solar_os_tui_move(solar_os_tui_t *tui, size_t row, size_t col)
{
    esp_err_t err = tui_validate_origin(tui, row, col);
    if (err != ESP_OK) {
        return err;
    }

    return solar_os_shell_io_set_cursor(tui->io, row, col);
}

esp_err_t solar_os_tui_set_cursor_visible(solar_os_tui_t *tui, bool visible)
{
    if (!tui_valid(tui)) {
        return ESP_ERR_INVALID_STATE;
    }

    return solar_os_shell_io_set_cursor_visible(tui->io, visible);
}

esp_err_t solar_os_tui_write(solar_os_tui_t *tui, const char *text, uint8_t attr)
{
    if (!tui_valid(tui) || text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool inverse = false;
    tui_save_attr(tui, &bold, &italic, &underline, &inverse);
    tui_set_attr(tui, attr);
    const esp_err_t err = tui_write_text(tui, text);
    tui_restore_attr(tui, bold, italic, underline, inverse);
    return err;
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

    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool inverse = false;
    tui_save_attr(tui, &bold, &italic, &underline, &inverse);
    tui_set_attr(tui, attr);
    const esp_err_t write_err = tui_write_codepoint(tui, codepoint);
    tui_restore_attr(tui, bold, italic, underline, inverse);
    return write_err;
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

    const size_t cols = tui_cols(tui);
    const size_t draw_width = col + width > cols ? cols - col : width;
    const uint32_t glyph = codepoint != 0 ? codepoint : TUI_BOX_H;

    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool inverse = false;
    tui_save_attr(tui, &bold, &italic, &underline, &inverse);
    tui_set_attr(tui, attr);
    err = solar_os_tui_move(tui, row, col);
    for (size_t i = 0; i < draw_width; i++) {
        if (err == ESP_OK) {
            err = tui_write_codepoint(tui, glyph);
        }
    }
    tui_restore_attr(tui, bold, italic, underline, inverse);
    return err;
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

    const size_t rows = tui_rows(tui);
    const size_t draw_height = row + height > rows ? rows - row : height;
    const uint32_t glyph = codepoint != 0 ? codepoint : TUI_BOX_V;

    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool inverse = false;
    tui_save_attr(tui, &bold, &italic, &underline, &inverse);
    tui_set_attr(tui, attr);
    esp_err_t write_err = ESP_OK;
    for (size_t i = 0; i < draw_height; i++) {
        esp_err_t err = solar_os_tui_move(tui, row + i, col);
        if (err == ESP_OK) {
            err = tui_write_codepoint(tui, glyph);
        }
        if (write_err == ESP_OK) {
            write_err = err;
        }
    }
    tui_restore_attr(tui, bold, italic, underline, inverse);
    return write_err;
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

    if (solar_os_shell_io_kind(tui->io) == SOLAR_OS_SHELL_IO_KIND_TERMINAL &&
        tui->terminal != NULL) {
        return solar_os_terminal_add_vrule(tui->terminal,
                                           row,
                                           col,
                                           height,
                                           width,
                                           (attr & SOLAR_OS_TUI_ATTR_INVERSE) != 0);
    }

    esp_err_t write_err = ESP_OK;
    for (uint8_t i = 0; i < width; i++) {
        const esp_err_t err = solar_os_tui_vline(tui, row, col + i, height, TUI_BOX_V, attr);
        if (write_err == ESP_OK) {
            write_err = err;
        }
    }
    return write_err;
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

    const size_t rows = tui_rows(tui);
    const size_t cols = tui_cols(tui);
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

    const size_t rows = tui_rows(tui);
    const size_t cols = tui_cols(tui);
    const size_t draw_height = row + height > rows ? rows - row : height;
    const size_t draw_width = col + width > cols ? cols - col : width;
    const uint32_t glyph = codepoint != 0 ? codepoint : ' ';

    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool inverse = false;
    tui_save_attr(tui, &bold, &italic, &underline, &inverse);
    tui_set_attr(tui, attr);
    esp_err_t write_err = ESP_OK;
    for (size_t y = 0; y < draw_height; y++) {
        esp_err_t err = solar_os_tui_move(tui, row + y, col);
        for (size_t x = 0; x < draw_width; x++) {
            if (err == ESP_OK) {
                err = tui_write_codepoint(tui, glyph);
            }
        }
        if (write_err == ESP_OK) {
            write_err = err;
        }
    }
    tui_restore_attr(tui, bold, italic, underline, inverse);
    return write_err;
}
