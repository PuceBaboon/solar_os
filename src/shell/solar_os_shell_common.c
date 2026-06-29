#include "solar_os_shell_common.h"

#include <errno.h>
#include <stdlib.h>

static solar_os_shell_io_t shell_command_fallback_io;

solar_os_shell_io_t *solar_os_shell_command_io(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL || solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_NONE) {
        solar_os_shell_io_init_terminal(&shell_command_fallback_io,
                                        solar_os_context_terminal(ctx));
        solar_os_context_set_shell_io(ctx, &shell_command_fallback_io);
        io = &shell_command_fallback_io;
    }
    return io;
}

solar_os_terminal_t *solar_os_shell_display_terminal(solar_os_context_t *ctx)
{
    return solar_os_context_terminal(ctx);
}

bool solar_os_shell_print_not_supported(solar_os_shell_io_t *term,
                                        const char *command,
                                        const char *feature,
                                        esp_err_t err)
{
    if (err != ESP_ERR_NOT_SUPPORTED) {
        return false;
    }

    solar_os_shell_io_printf(term,
                             "%s: %s not available on this board\n",
                             command,
                             feature);
    return true;
}

bool solar_os_shell_parse_u8(const char *text, uint8_t *value)
{
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || parsed > 0xff) {
        return false;
    }

    *value = (uint8_t)parsed;
    return true;
}

bool solar_os_shell_parse_size_arg(const char *text,
                                   size_t min,
                                   size_t max,
                                   size_t *value)
{
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || parsed < min || parsed > max) {
        return false;
    }

    *value = (size_t)parsed;
    return true;
}
