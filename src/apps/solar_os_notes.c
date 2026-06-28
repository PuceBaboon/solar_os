#include "solar_os_notes.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "solar_os_keys.h"
#include "solar_os_shell_io.h"
#include "solar_os_storage.h"

#define NOTES_DEFAULT_PATH "/.notes/default.md"
#define NOTES_DEFAULT_DIR "/.notes"
#define NOTES_MAX_CATEGORIES 64U
#define NOTES_MAX_ITEMS 256U
#define NOTES_MAX_VIEW_ROWS (NOTES_MAX_ITEMS + (NOTES_MAX_CATEGORIES * 2U))
#define NOTES_MAX_PREAMBLE_LINES 64U
#define NOTES_TEXT_MAX 128U
#define NOTES_LINE_MAX 192U
#define NOTES_MESSAGE_MAX 96U
#define NOTES_PREAMBLE_VISIBLE_MAX 3U

typedef enum {
    NOTES_INPUT_NONE,
    NOTES_INPUT_ADD_ITEM,
    NOTES_INPUT_EDIT_ITEM,
    NOTES_INPUT_ADD_CATEGORY,
    NOTES_INPUT_EDIT_CATEGORY,
} notes_input_mode_t;

typedef enum {
    NOTES_SELECT_NONE,
    NOTES_SELECT_CATEGORY,
    NOTES_SELECT_ITEM,
} notes_select_kind_t;

typedef enum {
    NOTES_VIEW_CATEGORY,
    NOTES_VIEW_ITEM,
    NOTES_VIEW_SEPARATOR,
} notes_view_type_t;

typedef struct {
    uint32_t id;
    bool collapsed;
    bool implicit;
    char text[NOTES_TEXT_MAX];
} notes_category_t;

typedef struct {
    uint32_t id;
    uint32_t category_id;
    bool checked;
    char text[NOTES_TEXT_MAX];
} notes_item_t;

typedef struct {
    notes_view_type_t type;
    size_t category;
    size_t item;
    uint32_t id;
} notes_view_row_t;

typedef struct {
    bool running;
    bool error_only;
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    char display_name[SOLAR_OS_STORAGE_PATH_MAX];
    char message[NOTES_MESSAGE_MAX];
    notes_category_t *categories;
    notes_item_t *items;
    notes_item_t *scratch;
    notes_view_row_t *view;
    char (*preamble)[NOTES_TEXT_MAX];
    size_t category_count;
    size_t item_count;
    size_t view_count;
    size_t preamble_count;
    size_t cursor;
    size_t top;
    uint32_t next_id;
    notes_input_mode_t input_mode;
    uint32_t input_item_id;
    uint32_t input_category_id;
    size_t input_insert_category;
    char input[NOTES_TEXT_MAX];
    size_t input_len;
    size_t input_cursor;
    size_t input_view_offset;
} notes_state_t;

static notes_state_t notes;
static solar_os_shell_io_t notes_fallback_io;

static solar_os_shell_io_t *notes_io(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL || solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_NONE) {
        solar_os_shell_io_init_terminal(&notes_fallback_io, solar_os_context_terminal(ctx));
        solar_os_context_set_shell_io(ctx, &notes_fallback_io);
        io = &notes_fallback_io;
    }
    return io;
}

static void notes_set_message(const char *message)
{
    strlcpy(notes.message, message != NULL ? message : "", sizeof(notes.message));
}

static void *notes_calloc(size_t count, size_t size)
{
    void *ptr = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == NULL) {
        ptr = heap_caps_calloc(count, size, MALLOC_CAP_8BIT);
    }
    return ptr;
}

static void notes_free_buffers(void)
{
    if (notes.categories != NULL) {
        heap_caps_free(notes.categories);
    }
    if (notes.items != NULL) {
        heap_caps_free(notes.items);
    }
    if (notes.scratch != NULL) {
        heap_caps_free(notes.scratch);
    }
    if (notes.view != NULL) {
        heap_caps_free(notes.view);
    }
    if (notes.preamble != NULL) {
        heap_caps_free(notes.preamble);
    }
    notes.categories = NULL;
    notes.items = NULL;
    notes.scratch = NULL;
    notes.view = NULL;
    notes.preamble = NULL;
}

static bool notes_alloc_buffers(void)
{
    notes.categories = notes_calloc(NOTES_MAX_CATEGORIES, sizeof(notes.categories[0]));
    notes.items = notes_calloc(NOTES_MAX_ITEMS, sizeof(notes.items[0]));
    notes.scratch = notes_calloc(NOTES_MAX_ITEMS, sizeof(notes.scratch[0]));
    notes.view = notes_calloc(NOTES_MAX_VIEW_ROWS, sizeof(notes.view[0]));
    notes.preamble = notes_calloc(NOTES_MAX_PREAMBLE_LINES, sizeof(notes.preamble[0]));
    if (notes.categories == NULL ||
        notes.items == NULL ||
        notes.scratch == NULL ||
        notes.view == NULL ||
        notes.preamble == NULL) {
        notes_free_buffers();
        return false;
    }
    return true;
}

static void notes_trim_line(char *line)
{
    if (line == NULL) {
        return;
    }
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1U] == '\n' || line[len - 1U] == '\r')) {
        line[--len] = '\0';
    }
}

static const char *notes_skip_space(const char *text)
{
    while (text != NULL && (*text == ' ' || *text == '\t')) {
        text++;
    }
    return text;
}

static bool notes_blank(const char *text)
{
    text = notes_skip_space(text);
    return text == NULL || *text == '\0';
}

static bool notes_parse_heading(const char *line, const char **text)
{
    const char *p = notes_skip_space(line);
    if (p == NULL || *p != '#') {
        return false;
    }

    size_t depth = 0;
    while (*p == '#' && depth < 6U) {
        p++;
        depth++;
    }
    if (depth == 0 || (*p != ' ' && *p != '\t')) {
        return false;
    }
    p = notes_skip_space(p);
    if (notes_blank(p)) {
        return false;
    }
    if (text != NULL) {
        *text = p;
    }
    return true;
}

static bool notes_parse_check_item(const char *line, bool *checked, const char **text)
{
    const char *p = notes_skip_space(line);
    if (p == NULL || (*p != '-' && *p != '*')) {
        return false;
    }
    p++;
    p = notes_skip_space(p);
    if (p[0] != '[' || p[2] != ']' || (p[1] != ' ' && p[1] != 'x' && p[1] != 'X')) {
        return false;
    }
    if (checked != NULL) {
        *checked = p[1] == 'x' || p[1] == 'X';
    }
    p += 3;
    p = notes_skip_space(p);
    if (text != NULL) {
        *text = p;
    }
    return true;
}

static bool notes_category_visible(const notes_category_t *category)
{
    return category != NULL && (!category->implicit || category->text[0] != '\0');
}

static bool notes_add_category_at(size_t index,
                                  const char *text,
                                  bool implicit,
                                  uint32_t *category_id)
{
    if (notes.category_count >= NOTES_MAX_CATEGORIES) {
        notes_set_message("category limit reached");
        return false;
    }
    if (index > notes.category_count) {
        index = notes.category_count;
    }

    for (size_t i = notes.category_count; i > index; i--) {
        notes.categories[i] = notes.categories[i - 1U];
    }

    notes_category_t *category = &notes.categories[index];
    memset(category, 0, sizeof(*category));
    category->id = notes.next_id++;
    category->implicit = implicit;
    strlcpy(category->text, text != NULL ? text : "", sizeof(category->text));
    notes.category_count++;
    if (category_id != NULL) {
        *category_id = category->id;
    }
    return true;
}

static size_t notes_find_category_index(uint32_t category_id)
{
    for (size_t i = 0; i < notes.category_count; i++) {
        if (notes.categories[i].id == category_id) {
            return i;
        }
    }
    return SIZE_MAX;
}

static uint32_t notes_default_category_id(void)
{
    for (size_t i = 0; i < notes.category_count; i++) {
        if (notes.categories[i].implicit && notes.categories[i].text[0] == '\0') {
            return notes.categories[i].id;
        }
    }

    uint32_t id = 0;
    (void)notes_add_category_at(0, "", true, &id);
    return id;
}

static bool notes_add_item(uint32_t category_id, bool checked, const char *text, uint32_t *item_id)
{
    if (notes.item_count >= NOTES_MAX_ITEMS) {
        notes_set_message("item limit reached");
        return false;
    }
    if (category_id == 0 || notes_find_category_index(category_id) == SIZE_MAX) {
        category_id = notes_default_category_id();
    }

    notes_item_t *item = &notes.items[notes.item_count++];
    memset(item, 0, sizeof(*item));
    item->id = notes.next_id++;
    item->category_id = category_id;
    item->checked = checked;
    strlcpy(item->text, text != NULL ? text : "", sizeof(item->text));
    if (item_id != NULL) {
        *item_id = item->id;
    }
    return true;
}

static size_t notes_find_item_index(uint32_t item_id)
{
    for (size_t i = 0; i < notes.item_count; i++) {
        if (notes.items[i].id == item_id) {
            return i;
        }
    }
    return SIZE_MAX;
}

static bool notes_view_add(notes_view_type_t type,
                           size_t category,
                           size_t item,
                           uint32_t id)
{
    if (notes.view_count >= NOTES_MAX_VIEW_ROWS) {
        return false;
    }
    notes.view[notes.view_count++] = (notes_view_row_t){
        .type = type,
        .category = category,
        .item = item,
        .id = id,
    };
    return true;
}

static void notes_cursor_to_selectable(void)
{
    if (notes.view_count == 0) {
        notes.cursor = 0;
        return;
    }
    if (notes.cursor >= notes.view_count) {
        notes.cursor = notes.view_count - 1U;
    }
    if (notes.view[notes.cursor].type != NOTES_VIEW_SEPARATOR) {
        return;
    }
    for (size_t i = notes.cursor + 1U; i < notes.view_count; i++) {
        if (notes.view[i].type != NOTES_VIEW_SEPARATOR) {
            notes.cursor = i;
            return;
        }
    }
    size_t i = notes.cursor;
    while (i > 0) {
        i--;
        if (notes.view[i].type != NOTES_VIEW_SEPARATOR) {
            notes.cursor = i;
            return;
        }
    }
    notes.cursor = 0;
}

static void notes_build_view(notes_select_kind_t keep_kind, uint32_t keep_id)
{
    notes.view_count = 0;
    size_t keep_index = SIZE_MAX;
    size_t hidden_parent_index = SIZE_MAX;

    for (size_t category = 0; category < notes.category_count; category++) {
        const notes_category_t *cat = &notes.categories[category];
        const bool category_visible = notes_category_visible(cat);
        const size_t category_view_index = notes.view_count;
        if (category_visible) {
            (void)notes_view_add(NOTES_VIEW_CATEGORY, category, SIZE_MAX, cat->id);
            if (keep_kind == NOTES_SELECT_CATEGORY && keep_id == cat->id) {
                keep_index = category_view_index;
            }
        }

        size_t unchecked = 0;
        size_t checked = 0;
        for (size_t item = 0; item < notes.item_count; item++) {
            if (notes.items[item].category_id != cat->id) {
                continue;
            }
            if (notes.items[item].checked) {
                checked++;
            } else {
                unchecked++;
            }
        }

        if (category_visible && cat->collapsed) {
            if (keep_kind == NOTES_SELECT_ITEM && hidden_parent_index == SIZE_MAX) {
                for (size_t item = 0; item < notes.item_count; item++) {
                    if (notes.items[item].category_id == cat->id && notes.items[item].id == keep_id) {
                        hidden_parent_index = category_view_index;
                        break;
                    }
                }
            }
            continue;
        }

        for (size_t item = 0; item < notes.item_count; item++) {
            if (notes.items[item].category_id != cat->id || notes.items[item].checked) {
                continue;
            }
            const size_t view_index = notes.view_count;
            (void)notes_view_add(NOTES_VIEW_ITEM, category, item, notes.items[item].id);
            if (keep_kind == NOTES_SELECT_ITEM && keep_id == notes.items[item].id) {
                keep_index = view_index;
            }
        }

        if (unchecked > 0 && checked > 0) {
            (void)notes_view_add(NOTES_VIEW_SEPARATOR, category, SIZE_MAX, cat->id);
        }

        for (size_t item = 0; item < notes.item_count; item++) {
            if (notes.items[item].category_id != cat->id || !notes.items[item].checked) {
                continue;
            }
            const size_t view_index = notes.view_count;
            (void)notes_view_add(NOTES_VIEW_ITEM, category, item, notes.items[item].id);
            if (keep_kind == NOTES_SELECT_ITEM && keep_id == notes.items[item].id) {
                keep_index = view_index;
            }
        }
    }

    if (keep_index == SIZE_MAX && hidden_parent_index != SIZE_MAX) {
        keep_index = hidden_parent_index;
    }
    if (keep_index != SIZE_MAX) {
        notes.cursor = keep_index;
    } else if (notes.view_count == 0) {
        notes.cursor = 0;
    } else if (notes.cursor >= notes.view_count) {
        notes.cursor = notes.view_count - 1U;
    }
    notes_cursor_to_selectable();
}

static const notes_view_row_t *notes_current_row(void)
{
    if (notes.cursor >= notes.view_count) {
        return NULL;
    }
    const notes_view_row_t *row = &notes.view[notes.cursor];
    return row->type != NOTES_VIEW_SEPARATOR ? row : NULL;
}

static void notes_reorder_items(notes_select_kind_t keep_kind, uint32_t keep_id)
{
    size_t out = 0;
    for (size_t category = 0; category < notes.category_count; category++) {
        const uint32_t category_id = notes.categories[category].id;
        for (size_t i = 0; i < notes.item_count; i++) {
            if (notes.items[i].category_id == category_id && !notes.items[i].checked) {
                notes.scratch[out++] = notes.items[i];
            }
        }
        for (size_t i = 0; i < notes.item_count; i++) {
            if (notes.items[i].category_id == category_id && notes.items[i].checked) {
                notes.scratch[out++] = notes.items[i];
            }
        }
    }
    if (out == notes.item_count) {
        memcpy(notes.items, notes.scratch, notes.item_count * sizeof(notes.items[0]));
    }
    notes_build_view(keep_kind, keep_id);
}

static size_t notes_preamble_visible_rows(size_t body_rows)
{
    size_t visible = notes.preamble_count;
    if (visible > NOTES_PREAMBLE_VISIBLE_MAX) {
        visible = NOTES_PREAMBLE_VISIBLE_MAX;
    }
    if (visible > body_rows) {
        visible = body_rows;
    }
    return visible;
}

static size_t notes_list_rows(solar_os_shell_io_t *io)
{
    const size_t rows = solar_os_shell_io_rows(io);
    if (rows <= 3) {
        return 0;
    }
    const size_t body = rows - 2U;
    const size_t preamble_rows = notes_preamble_visible_rows(body);
    return body > preamble_rows ? body - preamble_rows : 0;
}

static void notes_ensure_visible(solar_os_shell_io_t *io)
{
    const size_t list_rows = notes_list_rows(io);
    if (list_rows == 0 || notes.view_count == 0) {
        notes.top = 0;
        return;
    }

    if (notes.cursor < notes.top) {
        notes.top = notes.cursor;
    } else if (notes.cursor >= notes.top + list_rows) {
        notes.top = notes.cursor - list_rows + 1U;
    }

    if (notes.top >= notes.view_count) {
        notes.top = notes.view_count > 0 ? notes.view_count - 1U : 0;
    }
}

static void notes_write_clipped(solar_os_shell_io_t *io, const char *text, size_t width)
{
    if (width == 0) {
        return;
    }
    const size_t len = text != NULL ? strlen(text) : 0;
    const size_t write_len = len < width ? len : width;
    if (write_len > 0) {
        solar_os_shell_io_write_len(io, text, write_len);
    }
    for (size_t i = write_len; i < width; i++) {
        solar_os_shell_io_put_char(io, ' ');
    }
}

static void notes_input_ensure_visible(size_t width)
{
    if (width == 0) {
        notes.input_view_offset = notes.input_cursor;
        return;
    }
    if (notes.input_cursor < notes.input_view_offset) {
        notes.input_view_offset = notes.input_cursor;
    } else if (notes.input_cursor > notes.input_view_offset + width) {
        notes.input_view_offset = notes.input_cursor - width;
    }
}

static void notes_render_row(solar_os_shell_io_t *io, const notes_view_row_t *view, size_t cols)
{
    if (view == NULL) {
        notes_write_clipped(io, "", cols);
        return;
    }

    const bool selected = !notes.error_only && view == notes_current_row();
    char line[NOTES_LINE_MAX];

    if (view->type == NOTES_VIEW_CATEGORY) {
        const notes_category_t *cat = &notes.categories[view->category];
        const char marker = cat->collapsed ? '+' : '-';
        snprintf(line, sizeof(line), "%c %s", marker, cat->text);
        solar_os_shell_io_set_bold(io, true);
        solar_os_shell_io_set_inverse(io, selected);
        notes_write_clipped(io, line, cols);
        solar_os_shell_io_set_inverse(io, false);
        solar_os_shell_io_set_bold(io, false);
        return;
    }

    if (view->type == NOTES_VIEW_SEPARATOR) {
        const notes_category_t *cat = &notes.categories[view->category];
        notes_write_clipped(io, notes_category_visible(cat) ? "  ---- done ----" : "---- done ----", cols);
        return;
    }

    if (view->type == NOTES_VIEW_ITEM) {
        const notes_category_t *cat = &notes.categories[view->category];
        const notes_item_t *item = &notes.items[view->item];
        snprintf(line,
                 sizeof(line),
                 "%s[%c] %s",
                 notes_category_visible(cat) ? "  " : "",
                 item->checked ? 'x' : ' ',
                 item->text);
        solar_os_shell_io_set_inverse(io, selected);
        notes_write_clipped(io, line, cols);
        solar_os_shell_io_set_inverse(io, false);
    }
}

static const char *notes_input_label(void)
{
    switch (notes.input_mode) {
    case NOTES_INPUT_ADD_CATEGORY:
    case NOTES_INPUT_EDIT_CATEGORY:
        return "category: ";
    case NOTES_INPUT_ADD_ITEM:
        return "add: ";
    case NOTES_INPUT_EDIT_ITEM:
        return "edit: ";
    case NOTES_INPUT_NONE:
    default:
        return "";
    }
}

static void notes_render(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = notes_io(ctx);
    const size_t rows = solar_os_shell_io_rows(io);
    const size_t cols = solar_os_shell_io_cols(io);
    if (rows == 0 || cols == 0) {
        return;
    }

    notes_build_view(NOTES_SELECT_NONE, 0);
    notes_ensure_visible(io);
    solar_os_shell_io_clear(io);
    solar_os_shell_io_set_cursor_visible(io, false);

    solar_os_shell_io_set_cursor(io, 0, 0);
    solar_os_shell_io_set_inverse(io, true);
    char title[NOTES_LINE_MAX];
    snprintf(title,
             sizeof(title),
             "notes %s%s",
             notes.display_name[0] ? notes.display_name : notes.path,
             notes.error_only ? "" : "");
    notes_write_clipped(io, title, cols);
    solar_os_shell_io_set_inverse(io, false);

    size_t row = 1;
    const size_t body_rows = rows > 2 ? rows - 2U : 0U;
    const size_t preamble_rows = notes_preamble_visible_rows(body_rows);
    for (size_t i = 0; i < preamble_rows && row + 1U < rows; i++, row++) {
        solar_os_shell_io_set_cursor(io, row, 0);
        notes_write_clipped(io, notes.preamble[i], cols);
    }

    if (notes.view_count == 0 && !notes.error_only && row + 1U < rows) {
        solar_os_shell_io_set_cursor(io, row, 0);
        notes_write_clipped(io, "No checklist items", cols);
    }

    const size_t list_rows = rows > row + 1U ? rows - row - 1U : 0U;
    for (size_t visible = 0; visible < list_rows && notes.top + visible < notes.view_count; visible++) {
        solar_os_shell_io_set_cursor(io, row + visible, 0);
        notes_render_row(io, &notes.view[notes.top + visible], cols);
    }

    solar_os_shell_io_set_cursor(io, rows - 1U, 0);
    if (notes.input_mode != NOTES_INPUT_NONE) {
        const char *label = notes_input_label();
        solar_os_shell_io_write(io, label);
        const size_t label_len = strlen(label);
        const size_t input_width = cols > label_len ? cols - label_len : 0U;
        notes_input_ensure_visible(input_width);
        notes_write_clipped(io, &notes.input[notes.input_view_offset], input_width);
        solar_os_shell_io_set_cursor(io,
                                     rows - 1U,
                                     label_len + (notes.input_cursor >= notes.input_view_offset ?
                                         notes.input_cursor - notes.input_view_offset :
                                         0U));
        solar_os_shell_io_set_cursor_visible(io, true);
    } else {
        notes_write_clipped(io, notes.message, cols);
    }
    solar_os_shell_io_flush(io);
}

static bool notes_ensure_default_dir(void)
{
    char dir[SOLAR_OS_STORAGE_PATH_MAX];
    if (solar_os_storage_resolve_path(NOTES_DEFAULT_DIR, dir, sizeof(dir)) != ESP_OK) {
        return false;
    }
    if (solar_os_storage_mkdir(dir) == ESP_OK || errno == EEXIST) {
        return true;
    }
    return false;
}

static bool notes_file_blank_needed(bool wrote_content)
{
    return wrote_content;
}

static esp_err_t notes_save(void)
{
    FILE *file = fopen(notes.path, "w");
    if (file == NULL) {
        notes_set_message("save failed");
        return ESP_FAIL;
    }

    bool ok = true;
    bool wrote_content = false;
    for (size_t i = 0; i < notes.preamble_count; i++) {
        if (fprintf(file, "%s\n", notes.preamble[i]) < 0) {
            ok = false;
            break;
        }
        wrote_content = true;
    }

    for (size_t category = 0; ok && category < notes.category_count; category++) {
        const notes_category_t *cat = &notes.categories[category];
        const bool category_visible = notes_category_visible(cat);
        size_t category_items = 0;
        for (size_t item = 0; item < notes.item_count; item++) {
            if (notes.items[item].category_id == cat->id) {
                category_items++;
            }
        }
        if (!category_visible && category_items == 0) {
            continue;
        }

        if (category_visible) {
            if (notes_file_blank_needed(wrote_content) && fprintf(file, "\n") < 0) {
                ok = false;
                break;
            }
            ok = fprintf(file, "## %s\n", cat->text) >= 0;
            wrote_content = true;
        } else if (notes_file_blank_needed(wrote_content) && category_items > 0) {
            ok = fprintf(file, "\n") >= 0;
        }

        for (size_t item = 0; ok && item < notes.item_count; item++) {
            if (notes.items[item].category_id != cat->id) {
                continue;
            }
            ok = fprintf(file,
                         "- [%c] %s\n",
                         notes.items[item].checked ? 'x' : ' ',
                         notes.items[item].text) >= 0;
            wrote_content = true;
        }
    }

    if (fclose(file) != 0) {
        ok = false;
    }
    notes_set_message(ok ? "saved" : "save failed");
    return ok ? ESP_OK : ESP_FAIL;
}

static void notes_add_preamble(const char *line)
{
    if (notes.preamble_count >= NOTES_MAX_PREAMBLE_LINES) {
        return;
    }
    strlcpy(notes.preamble[notes.preamble_count++],
            line != NULL ? line : "",
            NOTES_TEXT_MAX);
}

static esp_err_t notes_load(void)
{
    FILE *file = fopen(notes.path, "r");
    if (file == NULL) {
        if (errno == ENOENT) {
            notes_set_message("new note");
            return ESP_OK;
        }
        notes_set_message("open failed");
        return ESP_FAIL;
    }

    uint32_t current_category = 0;
    bool seen_content = false;
    char line[NOTES_LINE_MAX];
    while (fgets(line, sizeof(line), file) != NULL) {
        notes_trim_line(line);

        const char *heading = NULL;
        bool checked = false;
        const char *text = NULL;
        if (notes_parse_heading(line, &heading)) {
            if (notes_add_category_at(notes.category_count, heading, false, &current_category)) {
                seen_content = true;
            }
        } else if (notes_parse_check_item(line, &checked, &text)) {
            if (current_category == 0) {
                current_category = notes_default_category_id();
            }
            (void)notes_add_item(current_category, checked, text, NULL);
            seen_content = true;
        } else if (!seen_content) {
            notes_add_preamble(line);
        } else if (!notes_blank(line)) {
            notes_add_preamble(line);
        }
    }
    const bool read_ok = !ferror(file);
    fclose(file);
    notes_reorder_items(NOTES_SELECT_NONE, 0);
    if (!read_ok) {
        notes_set_message("read failed");
        return ESP_FAIL;
    }
    if (notes.message[0] == '\0') {
        notes_set_message("");
    }
    return ESP_OK;
}

static uint32_t notes_selected_category_id(void)
{
    const notes_view_row_t *row = notes_current_row();
    if (row == NULL) {
        return notes_default_category_id();
    }
    if (row->type == NOTES_VIEW_CATEGORY || row->type == NOTES_VIEW_SEPARATOR) {
        return notes.categories[row->category].id;
    }
    if (row->type == NOTES_VIEW_ITEM) {
        return notes.items[row->item].category_id;
    }
    return notes_default_category_id();
}

static size_t notes_selected_category_insert_index(void)
{
    const notes_view_row_t *row = notes_current_row();
    if (row == NULL || row->category >= notes.category_count) {
        return notes.category_count;
    }
    return row->category + 1U;
}

static void notes_input_clear(notes_input_mode_t mode)
{
    notes.input_mode = mode;
    notes.input[0] = '\0';
    notes.input_len = 0;
    notes.input_cursor = 0;
    notes.input_view_offset = 0;
    notes.input_item_id = 0;
    notes.input_category_id = 0;
    notes.input_insert_category = notes.category_count;
}

static void notes_start_add_item(void)
{
    notes_input_clear(NOTES_INPUT_ADD_ITEM);
    notes.input_category_id = notes_selected_category_id();
    const size_t category = notes_find_category_index(notes.input_category_id);
    if (category != SIZE_MAX) {
        notes.categories[category].collapsed = false;
    }
}

static void notes_start_add_category(void)
{
    notes_input_clear(NOTES_INPUT_ADD_CATEGORY);
    notes.input_insert_category = notes_selected_category_insert_index();
}

static void notes_start_edit(void)
{
    const notes_view_row_t *row = notes_current_row();
    if (row == NULL) {
        return;
    }
    if (row->type == NOTES_VIEW_ITEM && row->item < notes.item_count) {
        notes_input_clear(NOTES_INPUT_EDIT_ITEM);
        notes.input_item_id = notes.items[row->item].id;
        strlcpy(notes.input, notes.items[row->item].text, sizeof(notes.input));
    } else if (row->type == NOTES_VIEW_CATEGORY && row->category < notes.category_count) {
        notes_input_clear(NOTES_INPUT_EDIT_CATEGORY);
        notes.input_category_id = notes.categories[row->category].id;
        strlcpy(notes.input, notes.categories[row->category].text, sizeof(notes.input));
    } else {
        return;
    }
    notes.input_len = strlen(notes.input);
    notes.input_cursor = notes.input_len;
}

static bool notes_is_printable(uint8_t ch)
{
    return isprint(ch) || ch >= 0xa0;
}

static void notes_finish_input(void)
{
    notes_select_kind_t keep_kind = NOTES_SELECT_NONE;
    uint32_t keep_id = 0;

    if (notes.input_mode == NOTES_INPUT_ADD_ITEM) {
        if (notes.input_len > 0) {
            uint32_t id = 0;
            if (notes_add_item(notes.input_category_id, false, notes.input, &id)) {
                notes_reorder_items(NOTES_SELECT_ITEM, id);
                keep_kind = NOTES_SELECT_ITEM;
                keep_id = id;
                (void)notes_save();
            }
        }
    } else if (notes.input_mode == NOTES_INPUT_EDIT_ITEM) {
        const size_t item = notes_find_item_index(notes.input_item_id);
        if (item != SIZE_MAX) {
            strlcpy(notes.items[item].text, notes.input, sizeof(notes.items[item].text));
            keep_kind = NOTES_SELECT_ITEM;
            keep_id = notes.items[item].id;
            (void)notes_save();
        }
    } else if (notes.input_mode == NOTES_INPUT_ADD_CATEGORY) {
        if (notes.input_len > 0) {
            uint32_t id = 0;
            if (notes_add_category_at(notes.input_insert_category, notes.input, false, &id)) {
                keep_kind = NOTES_SELECT_CATEGORY;
                keep_id = id;
                (void)notes_save();
            }
        }
    } else if (notes.input_mode == NOTES_INPUT_EDIT_CATEGORY) {
        const size_t category = notes_find_category_index(notes.input_category_id);
        if (category != SIZE_MAX && notes.input_len > 0) {
            strlcpy(notes.categories[category].text,
                    notes.input,
                    sizeof(notes.categories[category].text));
            notes.categories[category].implicit = false;
            keep_kind = NOTES_SELECT_CATEGORY;
            keep_id = notes.categories[category].id;
            (void)notes_save();
        }
    }

    notes.input_mode = NOTES_INPUT_NONE;
    notes.input[0] = '\0';
    notes.input_len = 0;
    notes.input_cursor = 0;
    notes.input_view_offset = 0;
    notes.input_item_id = 0;
    notes.input_category_id = 0;
    notes.input_insert_category = 0;
    notes_build_view(keep_kind, keep_id);
}

static void notes_cancel_input(void)
{
    notes.input_mode = NOTES_INPUT_NONE;
    notes.input[0] = '\0';
    notes.input_len = 0;
    notes.input_cursor = 0;
    notes.input_view_offset = 0;
    notes.input_item_id = 0;
    notes.input_category_id = 0;
    notes.input_insert_category = 0;
    notes_set_message("cancelled");
}

static bool notes_input_word_char(char ch)
{
    return !isspace((unsigned char)ch);
}

static void notes_input_word_left(void)
{
    size_t pos = notes.input_cursor;
    while (pos > 0 && isspace((unsigned char)notes.input[pos - 1U])) {
        pos--;
    }
    while (pos > 0 && notes_input_word_char(notes.input[pos - 1U])) {
        pos--;
    }
    notes.input_cursor = pos;
}

static void notes_input_word_right(void)
{
    size_t pos = notes.input_cursor;
    while (pos < notes.input_len && notes_input_word_char(notes.input[pos])) {
        pos++;
    }
    while (pos < notes.input_len && isspace((unsigned char)notes.input[pos])) {
        pos++;
    }
    notes.input_cursor = pos;
}

static void notes_input_backspace(void)
{
    if (notes.input_cursor == 0 || notes.input_len == 0) {
        return;
    }
    memmove(&notes.input[notes.input_cursor - 1U],
            &notes.input[notes.input_cursor],
            notes.input_len - notes.input_cursor + 1U);
    notes.input_cursor--;
    notes.input_len--;
}

static void notes_input_delete(void)
{
    if (notes.input_cursor >= notes.input_len) {
        return;
    }
    memmove(&notes.input[notes.input_cursor],
            &notes.input[notes.input_cursor + 1U],
            notes.input_len - notes.input_cursor);
    notes.input_len--;
}

static void notes_input_insert(uint8_t ch)
{
    if (!notes_is_printable(ch) || notes.input_len + 1U >= sizeof(notes.input)) {
        return;
    }
    memmove(&notes.input[notes.input_cursor + 1U],
            &notes.input[notes.input_cursor],
            notes.input_len - notes.input_cursor + 1U);
    notes.input[notes.input_cursor] = (char)ch;
    notes.input_cursor++;
    notes.input_len++;
}

static bool notes_handle_input(uint8_t ch)
{
    if (ch == SOLAR_OS_KEY_ESCAPE) {
        notes_cancel_input();
        return true;
    }
    if (ch == '\r' || ch == '\n') {
        notes_finish_input();
        return true;
    }
    if (ch == SOLAR_OS_KEY_LEFT) {
        if (notes.input_cursor > 0) {
            notes.input_cursor--;
        }
        return true;
    }
    if (ch == SOLAR_OS_KEY_RIGHT) {
        if (notes.input_cursor < notes.input_len) {
            notes.input_cursor++;
        }
        return true;
    }
    if (ch == SOLAR_OS_KEY_CTRL_LEFT) {
        notes_input_word_left();
        return true;
    }
    if (ch == SOLAR_OS_KEY_CTRL_RIGHT) {
        notes_input_word_right();
        return true;
    }
    if (ch == SOLAR_OS_KEY_HOME) {
        notes.input_cursor = 0;
        return true;
    }
    if (ch == SOLAR_OS_KEY_END) {
        notes.input_cursor = notes.input_len;
        return true;
    }
    if (ch == SOLAR_OS_KEY_DELETE) {
        notes_input_delete();
        return true;
    }
    if (ch == 0x08 || ch == 0x7f) {
        notes_input_backspace();
        return true;
    }
    notes_input_insert(ch);
    return true;
}

static bool notes_view_selectable(size_t index)
{
    return index < notes.view_count && notes.view[index].type != NOTES_VIEW_SEPARATOR;
}

static void notes_move_down(void)
{
    for (size_t i = notes.cursor + 1U; i < notes.view_count; i++) {
        if (notes_view_selectable(i)) {
            notes.cursor = i;
            return;
        }
    }
}

static void notes_move_up(void)
{
    size_t i = notes.cursor;
    while (i > 0) {
        i--;
        if (notes_view_selectable(i)) {
            notes.cursor = i;
            return;
        }
    }
}

static bool notes_find_move_target(size_t item, bool down, size_t *target)
{
    if (item >= notes.item_count || target == NULL) {
        return false;
    }

    const uint32_t category_id = notes.items[item].category_id;
    const bool checked = notes.items[item].checked;
    if (down) {
        for (size_t i = item + 1U; i < notes.item_count; i++) {
            if (notes.items[i].category_id == category_id && notes.items[i].checked == checked) {
                *target = i;
                return true;
            }
        }
    } else {
        size_t i = item;
        while (i > 0) {
            i--;
            if (notes.items[i].category_id == category_id && notes.items[i].checked == checked) {
                *target = i;
                return true;
            }
        }
    }
    return false;
}

static void notes_move_selected_item(bool down)
{
    const notes_view_row_t *row = notes_current_row();
    if (row == NULL || row->type != NOTES_VIEW_ITEM || row->item >= notes.item_count) {
        notes_set_message("select item");
        return;
    }

    size_t target = SIZE_MAX;
    if (!notes_find_move_target(row->item, down, &target)) {
        notes_set_message(down ? "bottom of section" : "top of section");
        return;
    }

    const uint32_t keep_id = notes.items[row->item].id;
    const notes_item_t item = notes.items[row->item];
    notes.items[row->item] = notes.items[target];
    notes.items[target] = item;
    notes_build_view(NOTES_SELECT_ITEM, keep_id);
    if (notes_save() == ESP_OK) {
        notes_set_message("moved");
    }
}

static void notes_page(solar_os_shell_io_t *io, bool down)
{
    const size_t step = notes_list_rows(io) > 0 ? notes_list_rows(io) : 1U;
    for (size_t i = 0; i < step; i++) {
        if (down) {
            notes_move_down();
        } else {
            notes_move_up();
        }
    }
}

static void notes_home(void)
{
    for (size_t i = 0; i < notes.view_count; i++) {
        if (notes_view_selectable(i)) {
            notes.cursor = i;
            return;
        }
    }
    notes.cursor = 0;
}

static void notes_end(void)
{
    size_t i = notes.view_count;
    while (i > 0) {
        i--;
        if (notes_view_selectable(i)) {
            notes.cursor = i;
            return;
        }
    }
    notes.cursor = 0;
}

static void notes_toggle_selected(void)
{
    const notes_view_row_t *row = notes_current_row();
    if (row == NULL) {
        return;
    }
    if (row->type == NOTES_VIEW_CATEGORY && row->category < notes.category_count) {
        notes.categories[row->category].collapsed = !notes.categories[row->category].collapsed;
        notes_build_view(NOTES_SELECT_CATEGORY, notes.categories[row->category].id);
        return;
    }
    if (row->type != NOTES_VIEW_ITEM || row->item >= notes.item_count) {
        return;
    }
    const uint32_t id = notes.items[row->item].id;
    notes.items[row->item].checked = !notes.items[row->item].checked;
    notes_reorder_items(NOTES_SELECT_ITEM, id);
    (void)notes_save();
}

static void notes_delete_category(size_t category)
{
    if (category >= notes.category_count) {
        return;
    }

    const uint32_t category_id = notes.categories[category].id;
    size_t out = 0;
    for (size_t i = 0; i < notes.item_count; i++) {
        if (notes.items[i].category_id != category_id) {
            notes.items[out++] = notes.items[i];
        }
    }
    notes.item_count = out;

    for (size_t i = category + 1U; i < notes.category_count; i++) {
        notes.categories[i - 1U] = notes.categories[i];
    }
    notes.category_count--;
    notes_build_view(NOTES_SELECT_NONE, 0);
    (void)notes_save();
}

static void notes_delete_selected(void)
{
    const notes_view_row_t *row = notes_current_row();
    if (row == NULL) {
        return;
    }
    if (row->type == NOTES_VIEW_CATEGORY) {
        notes_delete_category(row->category);
        return;
    }
    if (row->type != NOTES_VIEW_ITEM || row->item >= notes.item_count) {
        return;
    }

    for (size_t i = row->item + 1U; i < notes.item_count; i++) {
        notes.items[i - 1U] = notes.items[i];
    }
    notes.item_count--;
    notes_build_view(NOTES_SELECT_NONE, 0);
    (void)notes_save();
}

static void notes_collapse_selected(void)
{
    const notes_view_row_t *row = notes_current_row();
    if (row == NULL) {
        return;
    }

    size_t category = row->category;
    if (category >= notes.category_count || !notes_category_visible(&notes.categories[category])) {
        return;
    }

    notes.categories[category].collapsed = true;
    notes_build_view(NOTES_SELECT_CATEGORY, notes.categories[category].id);
}

static void notes_expand_selected(void)
{
    const notes_view_row_t *row = notes_current_row();
    if (row == NULL || row->type != NOTES_VIEW_CATEGORY || row->category >= notes.category_count) {
        return;
    }

    notes.categories[row->category].collapsed = false;
    notes_build_view(NOTES_SELECT_CATEGORY, notes.categories[row->category].id);
}

static esp_err_t notes_start(solar_os_context_t *ctx)
{
    memset(&notes, 0, sizeof(notes));
    notes.next_id = 1;
    if (!notes_alloc_buffers()) {
        return ESP_ERR_NO_MEM;
    }

    const int argc = solar_os_context_argc(ctx);
    if (argc > 2) {
        notes.error_only = true;
        notes_set_message("usage: notes [file.md]");
        notes_render(ctx);
        return ESP_OK;
    }

    if (!solar_os_storage_is_mounted()) {
        notes.error_only = true;
        notes_set_message("SD card not mounted");
        notes_render(ctx);
        return ESP_OK;
    }

    const char *arg = argc == 2 ? solar_os_context_argv(ctx, 1) : NOTES_DEFAULT_PATH;
    if (argc != 2) {
        (void)notes_ensure_default_dir();
    }
    strlcpy(notes.display_name, arg, sizeof(notes.display_name));

    const esp_err_t path_err = solar_os_storage_resolve_path(arg, notes.path, sizeof(notes.path));
    if (path_err != ESP_OK) {
        notes.error_only = true;
        notes_set_message(path_err == ESP_ERR_INVALID_SIZE ? "path too long" : "invalid path");
        notes_render(ctx);
        return ESP_OK;
    }

    const esp_err_t load_err = notes_load();
    if (load_err != ESP_OK) {
        notes.error_only = true;
    }
    notes.running = !notes.error_only;
    notes_render(ctx);
    return ESP_OK;
}

static void notes_stop(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = notes_io(ctx);
    solar_os_shell_io_set_cursor_visible(io, true);
    notes_free_buffers();
    memset(&notes, 0, sizeof(notes));
}

static bool notes_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL || event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }

    const uint8_t ch = (uint8_t)event->data.ch;
    if (ch == SOLAR_OS_KEY_APP_EXIT) {
        solar_os_context_request_exit(ctx);
        return true;
    }

    if (notes.error_only) {
        if (ch == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') {
            solar_os_context_request_exit(ctx);
        }
        return true;
    }

    notes_build_view(NOTES_SELECT_NONE, 0);
    if (notes.input_mode != NOTES_INPUT_NONE) {
        const bool handled = notes_handle_input(ch);
        notes_render(ctx);
        return handled;
    }

    switch (ch) {
    case SOLAR_OS_KEY_ESCAPE:
    case 'q':
    case 'Q':
        solar_os_context_request_exit(ctx);
        return true;
    case SOLAR_OS_KEY_UP:
        notes_move_up();
        break;
    case SOLAR_OS_KEY_DOWN:
        notes_move_down();
        break;
    case SOLAR_OS_KEY_SHIFT_UP:
        notes_move_selected_item(false);
        break;
    case SOLAR_OS_KEY_SHIFT_DOWN:
        notes_move_selected_item(true);
        break;
    case SOLAR_OS_KEY_LEFT:
        notes_collapse_selected();
        break;
    case SOLAR_OS_KEY_RIGHT:
        notes_expand_selected();
        break;
    case SOLAR_OS_KEY_PAGE_UP:
        notes_page(notes_io(ctx), false);
        break;
    case SOLAR_OS_KEY_PAGE_DOWN:
        notes_page(notes_io(ctx), true);
        break;
    case SOLAR_OS_KEY_HOME:
        notes_home();
        break;
    case SOLAR_OS_KEY_END:
        notes_end();
        break;
    case ' ':
        notes_toggle_selected();
        break;
    case 'a':
    case 'A':
        notes_start_add_item();
        break;
    case 'c':
    case 'C':
        notes_start_add_category();
        break;
    case 'd':
    case 'D':
    case SOLAR_OS_KEY_DELETE:
        notes_delete_selected();
        break;
    case '\r':
    case '\n':
        notes_start_edit();
        break;
    default:
        return true;
    }

    notes_render(ctx);
    return true;
}

const solar_os_app_t solar_os_notes_app = {
    .name = "notes",
    .summary = "Markdown checklist notes",
    .start = notes_start,
    .stop = notes_stop,
    .event = notes_event,
};
