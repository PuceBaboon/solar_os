#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os.h"
#include "solar_os_shell_io.h"
#include "solar_os_terminal.h"

solar_os_shell_io_t *solar_os_shell_command_io(solar_os_context_t *ctx);
solar_os_terminal_t *solar_os_shell_display_terminal(solar_os_context_t *ctx);

bool solar_os_shell_print_not_supported(solar_os_shell_io_t *term,
                                        const char *command,
                                        const char *feature,
                                        esp_err_t err);

bool solar_os_shell_parse_u8(const char *text, uint8_t *value);
bool solar_os_shell_parse_size_arg(const char *text,
                                   size_t min,
                                   size_t max,
                                   size_t *value);
