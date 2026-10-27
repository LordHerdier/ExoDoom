/*
 * shell_run_args.c — `run` argument text to launch params. See
 * shell_run_args.h.
 */

#include "shell/shell_run_args.h"

int shell_run_args_fill(const char *args, uint32_t params_len, uint8_t *buf)
{
    while (*args == ' ') {
        args++;
    }
    uint32_t n = 0;
    while (args[n] != '\0') {
        n++;
    }
    while (n > 0 && args[n - 1] == ' ') {
        n--;
    }

    if (params_len == 0) {
        return n == 0 ? SHELL_RUN_ARGS_OK : SHELL_RUN_ARGS_NO_PARAMS;
    }
    if (n >= params_len) {
        return SHELL_RUN_ARGS_TOO_LONG;
    }

    for (uint32_t i = 0; i < params_len; i++) {
        buf[i] = i < n ? (uint8_t)args[i] : 0;
    }
    return SHELL_RUN_ARGS_OK;
}
