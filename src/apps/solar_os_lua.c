#include "solar_os_lua.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
#include "solar_os_keys.h"
#include "solar_os_log.h"
#include "solar_os_shell_io.h"
#include "solar_os_storage.h"
#include "solar_os_terminal.h"

#define SOLUA_EVENT_QUEUE_LEN 24
#define SOLUA_INPUT_QUEUE_LEN 4
#define SOLUA_EVENT_DATA_MAX 128
#define SOLUA_REPL_INPUT_MAX 256
#define SOLUA_TASK_STACK 12288
#define SOLUA_TASK_PRIORITY 5
#define SOLUA_STOP_WAIT_MS 800
#define SOLUA_HOOK_INSTRUCTION_COUNT 10000
#define SOLUA_EXIT_MARKER "__solaros_lua_exit__"

typedef enum {
    SOLUA_EVENT_OUTPUT,
    SOLUA_EVENT_ERROR,
    SOLUA_EVENT_PROMPT,
    SOLUA_EVENT_DONE,
} solua_event_type_t;

typedef enum {
    SOLUA_MODE_REPL,
    SOLUA_MODE_SCRIPT,
} solua_mode_t;

typedef struct {
    solua_event_type_t type;
    bool success;
    size_t data_len;
    char data[SOLUA_EVENT_DATA_MAX];
} solua_event_t;

typedef struct {
    bool exit;
    char line[SOLUA_REPL_INPUT_MAX];
} solua_input_t;

typedef struct {
    solar_os_context_t *ctx;
    solar_os_shell_io_t fallback_io;
    QueueHandle_t events;
    QueueHandle_t input;
    TaskHandle_t task;
    solua_mode_t mode;
    bool running;
    bool task_done;
    bool stop_requested;
    bool interrupt_requested;
    bool interrupted;
    bool vm_active;
    bool repl_input_active;
    bool repl_executing;
    bool repl_exit_requested;
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    int argc;
    char argv[SOLAR_OS_APP_ARG_MAX][SOLAR_OS_APP_ARG_LEN];
    char repl_input[SOLUA_REPL_INPUT_MAX];
    size_t repl_input_len;
    size_t repl_input_cursor;
    size_t repl_input_row;
    size_t repl_input_col;
} solua_state_t;

static const char *TAG = "solar_os_lua";
static EXT_RAM_BSS_ATTR solua_state_t solua;

static solar_os_shell_io_t *solua_io(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL) {
        solar_os_shell_io_init_terminal(&solua.fallback_io, solar_os_context_terminal(ctx));
        solar_os_context_set_shell_io(ctx, &solua.fallback_io);
        io = &solua.fallback_io;
    }
    return io;
}

static void solua_return_to_shell(solar_os_context_t *ctx)
{
    solar_os_context_request_terminal_preserve(ctx);
    solar_os_context_request_exit(ctx);
}

static bool solua_send_event(const solua_event_t *event)
{
    if (event == NULL || solua.events == NULL) {
        return false;
    }

    while (!solua.stop_requested) {
        if (xQueueSend(solua.events, event, pdMS_TO_TICKS(50)) == pdPASS) {
            return true;
        }
    }
    return xQueueSend(solua.events, event, 0) == pdPASS;
}

static void solua_send_message(solua_event_type_t type, const char *message)
{
    solua_event_t event = {
        .type = type,
    };
    if (message != NULL) {
        strlcpy(event.data, message, sizeof(event.data));
        event.data_len = strlen(event.data);
    }
    (void)solua_send_event(&event);
}

static void solua_send_output(const char *data, size_t len)
{
    while (data != NULL && len > 0) {
        solua_event_t event = {
            .type = SOLUA_EVENT_OUTPUT,
        };
        const size_t chunk = len < sizeof(event.data) ? len : sizeof(event.data);
        memcpy(event.data, data, chunk);
        event.data_len = chunk;
        if (!solua_send_event(&event)) {
            return;
        }
        data += chunk;
        len -= chunk;
    }
}

static void solua_send_cstr_output(const char *text)
{
    if (text != NULL) {
        solua_send_output(text, strlen(text));
    }
}

static void *solua_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    (void)ud;
    (void)osize;

    if (nsize == 0) {
        heap_caps_free(ptr);
        return NULL;
    }

    void *next = NULL;
    if (ptr == NULL) {
        next = heap_caps_malloc(nsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (next == NULL) {
            next = heap_caps_malloc(nsize, MALLOC_CAP_8BIT);
        }
    } else {
        next = heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (next == NULL) {
            next = heap_caps_realloc(ptr, nsize, MALLOC_CAP_8BIT);
        }
    }
    return next;
}

static void solua_hook(lua_State *L, lua_Debug *ar)
{
    (void)ar;
    if (solua.stop_requested || solua.interrupt_requested) {
        luaL_error(L, "interrupted");
    }
}

static int solua_print(lua_State *L)
{
    const int top = lua_gettop(L);
    for (int i = 1; i <= top; i++) {
        if (i > 1) {
            solua_send_cstr_output("\t");
        }
        size_t len = 0;
        const char *text = luaL_tolstring(L, i, &len);
        solua_send_output(text, len);
        lua_pop(L, 1);
    }
    solua_send_cstr_output("\n");
    return 0;
}

static int solua_exit(lua_State *L)
{
    (void)L;
    solua.repl_exit_requested = true;
    return luaL_error(L, SOLUA_EXIT_MARKER);
}

static int solua_panic(lua_State *L)
{
    const char *message = lua_tostring(L, -1);
    solua_send_message(SOLUA_EVENT_ERROR, message != NULL ? message : "panic");
    return 0;
}

static void solua_open_libs(lua_State *L)
{
    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_DBLIBNAME, luaopen_debug, 1);
    lua_pop(L, 1);

    lua_pushcfunction(L, solua_print);
    lua_setglobal(L, "print");
    lua_pushcfunction(L, solua_exit);
    lua_setglobal(L, "exit");
}

static bool solua_is_exit_error(const char *message)
{
    return message != NULL && strstr(message, SOLUA_EXIT_MARKER) != NULL;
}

static void solua_report_error(const char *message)
{
    if (solua.stop_requested || solua.interrupt_requested) {
        solua.interrupted = true;
        return;
    }
    solua_send_message(SOLUA_EVENT_ERROR, message != NULL ? message : "unknown error");
}

static bool solua_call_loaded(lua_State *L, bool print_results)
{
    const int base = lua_gettop(L) - 1;
    solua.vm_active = true;
    const int status = lua_pcall(L, 0, print_results ? LUA_MULTRET : 0, 0);
    solua.vm_active = false;
    if (status != LUA_OK) {
        const char *message = lua_tostring(L, -1);
        if (solua_is_exit_error(message)) {
            lua_pop(L, 1);
            return true;
        }
        solua_report_error(message);
        lua_pop(L, 1);
        return false;
    }

    if (print_results) {
        const int top = lua_gettop(L);
        for (int i = base + 1; i <= top; i++) {
            if (i > base + 1) {
                solua_send_cstr_output("\t");
            }
            size_t len = 0;
            const char *text = luaL_tolstring(L, i, &len);
            solua_send_output(text, len);
            lua_pop(L, 1);
        }
        if (top > base) {
            solua_send_cstr_output("\n");
        }
        lua_settop(L, base);
    }
    return true;
}

static int solua_load_repl_line(lua_State *L, const char *line)
{
    while (isspace((unsigned char)*line)) {
        line++;
    }

    const char *expr = line;
    if (*expr == '=') {
        expr++;
        while (isspace((unsigned char)*expr)) {
            expr++;
        }
    }

    char chunk[SOLUA_REPL_INPUT_MAX + 8];
    const int written = snprintf(chunk, sizeof(chunk), "return %s", expr);
    if (written > 0 && (size_t)written < sizeof(chunk)) {
        const int status = luaL_loadbufferx(L, chunk, strlen(chunk), "=stdin", "t");
        if (status == LUA_OK) {
            return status;
        }
        lua_pop(L, 1);
    }

    return luaL_loadbufferx(L, line, strlen(line), "=stdin", "t");
}

static void solua_set_args(lua_State *L)
{
    lua_newtable(L);
    for (int i = 0; i < solua.argc; i++) {
        lua_pushinteger(L, i);
        lua_pushstring(L, solua.argv[i]);
        lua_settable(L, -3);
    }
    lua_setglobal(L, "arg");
}

static bool solua_run_script(lua_State *L)
{
    solua_set_args(L);
    int status = luaL_loadfilex(L, solua.path, "t");
    if (status != LUA_OK) {
        solua_report_error(lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    return solua_call_loaded(L, false);
}

static void solua_run_repl(lua_State *L)
{
    solua_send_message(SOLUA_EVENT_PROMPT, "> ");
    while (!solua.stop_requested && !solua.repl_exit_requested) {
        solua_input_t input = {0};
        if (xQueueReceive(solua.input, &input, portMAX_DELAY) != pdPASS) {
            continue;
        }
        if (input.exit || solua.stop_requested) {
            break;
        }

        char *line = input.line;
        while (isspace((unsigned char)*line)) {
            line++;
        }
        if (*line == '\0') {
            solua_send_message(SOLUA_EVENT_PROMPT, "> ");
            continue;
        }

        solua.repl_executing = true;
        solua.interrupted = false;
        const int status = solua_load_repl_line(L, line);
        if (status == LUA_OK) {
            (void)solua_call_loaded(L, true);
        } else {
            solua_report_error(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        solua.interrupt_requested = false;
        solua.repl_executing = false;

        if (!solua.stop_requested && !solua.repl_exit_requested) {
            solua_send_message(SOLUA_EVENT_PROMPT, "> ");
        }
    }
}

static void solua_task(void *arg)
{
    (void)arg;

    SOLAR_OS_LOGI(TAG, "task start: mode=%s", solua.mode == SOLUA_MODE_REPL ? "repl" : "script");
    lua_State *L = lua_newstate(solua_alloc, NULL);
    bool success = false;
    if (L == NULL) {
        solua_send_message(SOLUA_EVENT_ERROR, "out of memory");
        goto done;
    }

    lua_atpanic(L, solua_panic);
    solua_open_libs(L);
    lua_sethook(L, solua_hook, LUA_MASKCOUNT, SOLUA_HOOK_INSTRUCTION_COUNT);

    if (solua.mode == SOLUA_MODE_SCRIPT) {
        success = solua_run_script(L);
    } else {
        solua_run_repl(L);
        success = !solua.interrupted || solua.repl_exit_requested;
    }

done:
    if (L != NULL) {
        lua_close(L);
    }

    solua_event_t event = {
        .type = SOLUA_EVENT_DONE,
        .success = success,
    };
    (void)solua_send_event(&event);
    solua.task_done = true;
    solua.task = NULL;
    SOLAR_OS_LOGI(TAG, "task stop: success=%d", success);
    vTaskDelete(NULL);
}

static void solua_render_usage(solar_os_shell_io_t *io)
{
    solar_os_shell_io_writeln(io, "usage: lua [file.lua] [args...]");
    solar_os_shell_io_writeln(io, "  lua");
    solar_os_shell_io_writeln(io, "  lua hello.lua");
    solar_os_shell_io_writeln(io, "  lua /sdcard/apps/demo/main.lua arg");
}

static bool solua_path_has_suffix(const char *path, const char *suffix)
{
    const size_t path_len = path != NULL ? strlen(path) : 0;
    const size_t suffix_len = suffix != NULL ? strlen(suffix) : 0;
    return path_len >= suffix_len &&
        suffix_len > 0 &&
        strcmp(path + path_len - suffix_len, suffix) == 0;
}

static void solua_finish_terminal_line(solar_os_shell_io_t *io)
{
    if (io != NULL && solar_os_shell_io_cursor_col(io) != 0) {
        solar_os_shell_io_newline(io);
        solar_os_shell_io_flush(io);
    }
}

static esp_err_t solua_start(solar_os_context_t *ctx)
{
    memset(&solua, 0, sizeof(solua));
    solua.ctx = ctx;

    solar_os_shell_io_t *io = solua_io(ctx);
    const int argc = solar_os_context_argc(ctx);
    if (argc > SOLAR_OS_APP_ARG_MAX) {
        solar_os_shell_io_writeln(io, "lua: too many arguments");
        solar_os_shell_io_flush(io);
        solua_return_to_shell(ctx);
        return ESP_OK;
    }

    const bool repl_mode = argc < 2;
    solua.mode = repl_mode ? SOLUA_MODE_REPL : SOLUA_MODE_SCRIPT;
    solua.argc = repl_mode ? 1 : argc - 1;
    strlcpy(solua.argv[0], repl_mode ? "lua" : solar_os_context_argv(ctx, 1), sizeof(solua.argv[0]));

    if (repl_mode) {
        solar_os_shell_io_clear(io);
        solar_os_shell_io_write_bold(io, LUA_RELEASE " on SolarOS");
        solar_os_shell_io_newline(io);
        solar_os_shell_io_writeln(io, "exit() returns to shell");
        solar_os_shell_io_printf(io, "%s exits\n", solar_os_shell_io_app_exit_key(io));
        solar_os_shell_io_flush(io);
    } else {
        const char *script_arg = solar_os_context_argv(ctx, 1);
        if (script_arg == NULL || script_arg[0] == '\0') {
            solua_render_usage(io);
            solua_return_to_shell(ctx);
            return ESP_OK;
        }

        esp_err_t path_err = solar_os_storage_resolve_path(script_arg,
                                                           solua.path,
                                                           sizeof(solua.path));
        if (path_err != ESP_OK) {
            solar_os_shell_io_printf(io, "lua: invalid path: %s\n", esp_err_to_name(path_err));
            solar_os_shell_io_flush(io);
            solua_return_to_shell(ctx);
            return ESP_OK;
        }
        if (!solua_path_has_suffix(solua.path, ".lua")) {
            solar_os_shell_io_writeln(io, "lua: expected .lua file");
            solar_os_shell_io_flush(io);
            solua_return_to_shell(ctx);
            return ESP_OK;
        }

        struct stat st;
        if (stat(solua.path, &st) != 0 || !S_ISREG(st.st_mode)) {
            solar_os_shell_io_printf(io, "lua: not found: %s\n", solua.path);
            solar_os_shell_io_flush(io);
            solua_return_to_shell(ctx);
            return ESP_OK;
        }

        for (int i = 1; i < argc; i++) {
            strlcpy(solua.argv[i - 1],
                    solar_os_context_argv(ctx, i),
                    sizeof(solua.argv[i - 1]));
        }
        strlcpy(solua.argv[0], solua.path, sizeof(solua.argv[0]));
    }

    solua.events = xQueueCreate(SOLUA_EVENT_QUEUE_LEN, sizeof(solua_event_t));
    if (solua.events == NULL) {
        solar_os_shell_io_writeln(io, "lua: out of memory");
        solar_os_shell_io_flush(io);
        if (!repl_mode) {
            solua_return_to_shell(ctx);
        }
        return ESP_OK;
    }

    if (repl_mode) {
        solua.input = xQueueCreate(SOLUA_INPUT_QUEUE_LEN, sizeof(solua_input_t));
        if (solua.input == NULL) {
            vQueueDelete(solua.events);
            solua.events = NULL;
            solar_os_shell_io_writeln(io, "lua: out of memory");
            solar_os_shell_io_flush(io);
            return ESP_OK;
        }
    }

    solua.running = true;
    const BaseType_t created = xTaskCreatePinnedToCore(solua_task,
                                                       "solar_os_lua",
                                                       SOLUA_TASK_STACK,
                                                       NULL,
                                                       SOLUA_TASK_PRIORITY,
                                                       &solua.task,
                                                       tskNO_AFFINITY);
    if (created != pdPASS) {
        if (solua.input != NULL) {
            vQueueDelete(solua.input);
            solua.input = NULL;
        }
        vQueueDelete(solua.events);
        solua.events = NULL;
        solua.running = false;
        solar_os_shell_io_writeln(io, "lua: task create failed");
        solar_os_shell_io_flush(io);
        if (!repl_mode) {
            solua_return_to_shell(ctx);
        }
    }

    return ESP_OK;
}

static void solua_interrupt_current(void)
{
    solua.interrupt_requested = true;
    solua.interrupted = true;
}

static void solua_stop(solar_os_context_t *ctx)
{
    (void)ctx;

    solua.stop_requested = true;
    if (solua.input != NULL) {
        solua_input_t input = {
            .exit = true,
        };
        (void)xQueueSend(solua.input, &input, 0);
    }

    if (solua.task != NULL && !solua.task_done) {
        const TickType_t start = xTaskGetTickCount();
        while (solua.task != NULL &&
               !solua.task_done &&
               (xTaskGetTickCount() - start) < pdMS_TO_TICKS(SOLUA_STOP_WAIT_MS)) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (solua.task != NULL && !solua.task_done) {
            SOLAR_OS_LOGW(TAG, "force stopping unresponsive Lua task");
            vTaskDelete(solua.task);
            solua.task = NULL;
            solua.task_done = true;
            solua.vm_active = false;
        }
    }

    if (solua.events != NULL) {
        vQueueDelete(solua.events);
        solua.events = NULL;
    }
    if (solua.input != NULL) {
        vQueueDelete(solua.input);
        solua.input = NULL;
    }
}

static bool solua_is_printable_char(char ch)
{
    const unsigned char uch = (unsigned char)ch;
    return uch >= 0x20 && uch < 0x7f;
}

static size_t solua_repl_max_input_len(solar_os_context_t *ctx)
{
    (void)ctx;
    return sizeof(solua.repl_input) - 1;
}

static void solua_repl_render_input(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = solua_io(ctx);
    solar_os_shell_io_clear_line_from(io, solua.repl_input_row, solua.repl_input_col);
    solar_os_shell_io_set_cursor(io, solua.repl_input_row, solua.repl_input_col);
    solar_os_shell_io_write_len(io, solua.repl_input, solua.repl_input_len);
    solar_os_shell_io_set_cursor(io,
                                 solua.repl_input_row,
                                 solua.repl_input_col + solua.repl_input_cursor);
    solar_os_shell_io_flush(io);
}

static void solua_repl_move_cursor_left(solar_os_context_t *ctx)
{
    if (solua.repl_input_cursor > 0) {
        solua.repl_input_cursor--;
        solua_repl_render_input(ctx);
    }
}

static void solua_repl_move_cursor_right(solar_os_context_t *ctx)
{
    if (solua.repl_input_cursor < solua.repl_input_len) {
        solua.repl_input_cursor++;
        solua_repl_render_input(ctx);
    }
}

static void solua_repl_move_cursor_home(solar_os_context_t *ctx)
{
    if (solua.repl_input_cursor != 0) {
        solua.repl_input_cursor = 0;
        solua_repl_render_input(ctx);
    }
}

static void solua_repl_move_cursor_end(solar_os_context_t *ctx)
{
    if (solua.repl_input_cursor != solua.repl_input_len) {
        solua.repl_input_cursor = solua.repl_input_len;
        solua_repl_render_input(ctx);
    }
}

static void solua_repl_insert_char(solar_os_context_t *ctx, char ch)
{
    if (solua.repl_input_len >= solua_repl_max_input_len(ctx)) {
        return;
    }
    memmove(&solua.repl_input[solua.repl_input_cursor + 1],
            &solua.repl_input[solua.repl_input_cursor],
            solua.repl_input_len - solua.repl_input_cursor + 1);
    solua.repl_input[solua.repl_input_cursor++] = ch;
    solua.repl_input_len++;
    solua_repl_render_input(ctx);
}

static void solua_repl_backspace(solar_os_context_t *ctx)
{
    if (solua.repl_input_cursor == 0) {
        return;
    }
    memmove(&solua.repl_input[solua.repl_input_cursor - 1],
            &solua.repl_input[solua.repl_input_cursor],
            solua.repl_input_len - solua.repl_input_cursor + 1);
    solua.repl_input_cursor--;
    solua.repl_input_len--;
    solua_repl_render_input(ctx);
}

static void solua_repl_delete(solar_os_context_t *ctx)
{
    if (solua.repl_input_cursor >= solua.repl_input_len) {
        return;
    }
    memmove(&solua.repl_input[solua.repl_input_cursor],
            &solua.repl_input[solua.repl_input_cursor + 1],
            solua.repl_input_len - solua.repl_input_cursor);
    solua.repl_input_len--;
    solua_repl_render_input(ctx);
}

static void solua_repl_submit(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = solua_io(ctx);
    solar_os_shell_io_newline(io);
    solar_os_shell_io_flush(io);

    solua_input_t input = {0};
    strlcpy(input.line, solua.repl_input, sizeof(input.line));
    solua.repl_input_active = false;
    solua.repl_input_len = 0;
    solua.repl_input_cursor = 0;
    solua.repl_input[0] = '\0';

    if (solua.input == NULL || xQueueSend(solua.input, &input, 0) != pdPASS) {
        solar_os_shell_io_writeln(io, "lua: input queue full");
        solar_os_shell_io_flush(io);
        solua.repl_input_active = true;
    }
}

static void solua_drain_events(solar_os_context_t *ctx)
{
    if (solua.events == NULL) {
        return;
    }

    solar_os_shell_io_t *io = solua_io(ctx);
    solua_event_t event;
    uint32_t drained = 0;
    while (drained++ < 24 && xQueueReceive(solua.events, &event, 0) == pdPASS) {
        switch (event.type) {
        case SOLUA_EVENT_OUTPUT:
            for (size_t i = 0; i < event.data_len; i++) {
                solar_os_shell_io_put_utf8_byte(io, (uint8_t)event.data[i]);
            }
            break;
        case SOLUA_EVENT_ERROR:
            solar_os_shell_io_printf(io, "lua: %s\n", event.data);
            break;
        case SOLUA_EVENT_PROMPT:
            solua.repl_input_active = true;
            solua.repl_input_len = 0;
            solua.repl_input_cursor = 0;
            solua.repl_input[0] = '\0';
            solar_os_shell_io_write(io, event.data_len > 0 ? event.data : "> ");
            solua.repl_input_row = solar_os_shell_io_cursor_row(io);
            solua.repl_input_col = solar_os_shell_io_cursor_col(io);
            break;
        case SOLUA_EVENT_DONE:
            solua.running = false;
            solua.task_done = true;
            if (solua.mode == SOLUA_MODE_SCRIPT || solua.repl_exit_requested) {
                solua_finish_terminal_line(io);
                if (!event.success && !solua.interrupted) {
                    solar_os_shell_io_writeln(io, "lua: failed");
                } else if (!event.success) {
                    solar_os_shell_io_writeln(io, "lua: stopped");
                }
                solar_os_shell_io_flush(io);
                solua_return_to_shell(ctx);
                break;
            }
            solar_os_shell_io_printf(io, "lua: %s\n", event.success ? "done" : "stopped");
            solar_os_shell_io_printf(io, "%s exits\n", solar_os_shell_io_app_exit_key(io));
            break;
        default:
            break;
        }
    }
    solar_os_shell_io_flush(io);
}

static bool solua_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }

    if (event->type == SOLAR_OS_EVENT_TICK) {
        solua_drain_events(ctx);
        return true;
    }
    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }

    const uint8_t ch = (uint8_t)event->data.ch;
    if (ch == SOLAR_OS_KEY_APP_EXIT) {
        if (solua.mode == SOLUA_MODE_REPL && solua.repl_executing && !solua.interrupt_requested) {
            solar_os_shell_io_t *io = solua_io(ctx);
            solar_os_shell_io_writeln(io, "\nlua: interrupt");
            solar_os_shell_io_flush(io);
            solua_interrupt_current();
            return true;
        }
        if (solua.mode == SOLUA_MODE_SCRIPT && solua.running && !solua.stop_requested) {
            solar_os_shell_io_t *io = solua_io(ctx);
            solar_os_shell_io_writeln(io, "\nlua: interrupt");
            solar_os_shell_io_flush(io);
            solua.stop_requested = true;
        }
        solua_return_to_shell(ctx);
        return true;
    }

    if (ch == SOLAR_OS_KEY_PAGE_UP) {
        solar_os_terminal_t *term = solar_os_shell_io_terminal(solua_io(ctx));
        if (term != NULL) {
            solar_os_terminal_page_up(term);
        }
        return true;
    }
    if (ch == SOLAR_OS_KEY_PAGE_DOWN) {
        solar_os_terminal_t *term = solar_os_shell_io_terminal(solua_io(ctx));
        if (term != NULL) {
            solar_os_terminal_page_down(term);
        }
        return true;
    }
    if (solua.mode != SOLUA_MODE_REPL || !solua.repl_input_active) {
        return true;
    }

    switch (ch) {
    case SOLAR_OS_KEY_LEFT:
        solua_repl_move_cursor_left(ctx);
        break;
    case SOLAR_OS_KEY_RIGHT:
        solua_repl_move_cursor_right(ctx);
        break;
    case SOLAR_OS_KEY_HOME:
    case SOLAR_OS_KEY_CTRL_HOME:
        solua_repl_move_cursor_home(ctx);
        break;
    case SOLAR_OS_KEY_END:
    case SOLAR_OS_KEY_CTRL_END:
        solua_repl_move_cursor_end(ctx);
        break;
    case SOLAR_OS_KEY_DELETE:
        solua_repl_delete(ctx);
        break;
    case SOLAR_OS_KEY_ESCAPE:
        if (solua.repl_input_len > 0) {
            solua.repl_input_len = 0;
            solua.repl_input_cursor = 0;
            solua.repl_input[0] = '\0';
            solua_repl_render_input(ctx);
        }
        break;
    case '\r':
    case '\n':
        solua_repl_submit(ctx);
        break;
    case '\b':
        solua_repl_backspace(ctx);
        break;
    default:
        if (solua_is_printable_char((char)ch)) {
            solua_repl_insert_char(ctx, (char)ch);
        }
        break;
    }

    return true;
}

const solar_os_app_t solar_os_lua_app = {
    .name = "lua",
    .summary = "Lua runtime",
    .start = solua_start,
    .stop = solua_stop,
    .event = solua_event,
};
