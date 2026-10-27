/*
 * doom_args.c — launch params text to Doom's argv. See doom_args.h.
 */

#include "doom_args.h"

#include <stddef.h>

/* Next token of text[*pos, len): its start and length, or 0 at the end. */
static int next_token(const char *text, uint32_t len, uint32_t *pos,
                      uint32_t *start, uint32_t *tok_len)
{
    uint32_t i = *pos;
    while (i < len && text[i] == ' ') {
        i++;
    }
    if (i >= len || text[i] == '\0') {
        *pos = i;
        return 0;
    }
    *start = i;
    while (i < len && text[i] != ' ' && text[i] != '\0') {
        i++;
    }
    *tok_len = i - *start;
    *pos = i;
    return 1;
}

int doom_args_split(const char *prog, const char *text, uint32_t text_len,
                    char *store, uint32_t store_len, char **argv, int argv_slots)
{
    argv[0] = (char *)prog;
    if (argv_slots >= 2) {
        argv[1] = NULL;
    }
    if (argv_slots < DOOM_ARGV_SLOTS(text_len)) {
        return -1;
    }

    int argc = 1;
    uint32_t pos = 0, used = 0, start, n;
    while (next_token(text, text_len, &pos, &start, &n)) {
        if (used + n + 1 > store_len) {
            argv[argc] = NULL;
            return -1;
        }
        argv[argc++] = store + used;
        for (uint32_t i = 0; i < n; i++) {
            store[used++] = text[start + i];
        }
        store[used++] = '\0';
    }
    argv[argc] = NULL;
    return argc;
}

static int token_is(const char *text, uint32_t start, uint32_t n,
                    const char *s)
{
    uint32_t i = 0;
    for (; i < n; i++) {
        if (s[i] != text[start + i]) {
            return 0;
        }
    }
    return s[i] == '\0';
}

int doom_args_value(const char *text, uint32_t text_len, const char *flag,
                    char *out, uint32_t n)
{
    uint32_t pos = 0, start, len;
    out[0] = '\0';
    while (next_token(text, text_len, &pos, &start, &len)) {
        if (!token_is(text, start, len, flag)) {
            continue;
        }
        if (!next_token(text, text_len, &pos, &start, &len) || len + 1 > n) {
            return 0;
        }
        for (uint32_t i = 0; i < len; i++) {
            out[i] = text[start + i];
        }
        out[len] = '\0';
        return 1;
    }
    return 0;
}
