#pragma once

#include <stdint.h>

/*
 * doom_args — Doom's command line from the launch params (SCRUM-301).
 *
 * `run doom -iwad doom2.wad` reaches Doom as one NUL-terminated string in the
 * launch params the shell filled in (exo_launch_pkg_params, #36). Doom wants
 * an argv: this turns the string into one, and finds the value of a flag
 * without needing one. Pure string handling: no syscalls, no libc, no
 * allocation, so it sits in src/ and links into the kernel (where
 * DG_Init's IWAD check uses it and tests/kernel/test_doom_args_k.c drives it)
 * and into the ring-3 Doom image alike.
 */

/* Room for an argument string of `text_len` bytes: the program name, the
 * most tokens that many bytes can hold (one per two bytes: "a a a ..."), and
 * the terminating NULL. doom_args_split() cannot run out. */
#define DOOM_ARGV_SLOTS(text_len) ((int)((text_len) / 2u) + 2)

/*
 * Split `text` on spaces into argv[1..], with argv[0] = `prog`. The tokens
 * are copied into store[0..store_len), NUL-terminated, so `text` is left as
 * it was (DG_Init reads it again after this has run). argv ends in NULL.
 * Returns argc (>= 1), or -1 if argv has fewer than DOOM_ARGV_SLOTS(len)
 * slots for `text`'s length or `store` is shorter than text_len + 1 (always
 * enough), in which case argv[0] is still `prog` and argv[1] is NULL.
 *
 * `text` need not be terminated inside `text_len`: at most `text_len` bytes
 * are read, and a NUL ends it earlier.
 */
int doom_args_split(const char *prog, const char *text, uint32_t text_len,
                    char *store, uint32_t store_len, char **argv, int argv_slots);

/*
 * The token after `flag` in `text` (read the same way), copied to out[0..n)
 * NUL-terminated. Returns 1 if found with a value, else 0 with out[0] = 0:
 * no such flag, the flag is last, or the value does not fit in `out`.
 */
int doom_args_value(const char *text, uint32_t text_len, const char *flag,
                    char *out, uint32_t n);
