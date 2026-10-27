/*
 * shell_hist.h — the shell's command history (SCRUM-273).
 *
 * A ring of the last SHELL_HIST_MAX submitted lines that Up/Down walk. Pure
 * string logic -- no console, no syscalls, no libc -- so the same TU links
 * into the ring-3 shell blob and, under TESTING=1, into the kernel for
 * tests/kernel/test_shell_hist_k.c (docker/scripts/build.sh), the
 * arrangement shell_mount.c and shell_err.c already use.
 *
 * The caller owns the redraw: prev/next hand back the line to show, and the
 * shell erases and rewrites the input row itself.
 */
#ifndef EXODOOM_SHELL_HIST_H
#define EXODOOM_SHELL_HIST_H

#include <stddef.h>

/* Entries kept; the oldest is dropped when a 17th is pushed. */
#define SHELL_HIST_MAX 16u

/* Bytes per entry, NUL included. Equal to the shell's SHELL_CMD_BUF_LEN
 * (shell_main.c checks this at build time). */
#define SHELL_HIST_LEN 64u

typedef struct {
    char     ent[SHELL_HIST_MAX][SHELL_HIST_LEN];
    unsigned count;            /* entries held, <= SHELL_HIST_MAX */
    unsigned next;             /* slot the next push writes */
    unsigned pos;              /* entries back from the newest while browsing */
    int      browsing;         /* non-zero between the first Up and Enter */
    char     draft[SHELL_HIST_LEN]; /* the line being typed when Up began */
} shell_hist_t;

void shell_hist_init(shell_hist_t *h);

/* Record a submitted line and stop browsing. An empty line, or one equal to
 * the newest entry, is not stored. Longer than SHELL_HIST_LEN - 1 is cut. */
void shell_hist_push(shell_hist_t *h, const char *line);

/* Up. `current` is the line on screen; it is kept as the draft on the first
 * call of a browse. Returns the next older entry (the oldest again once
 * there is nothing older), or NULL if the history is empty. */
const char *shell_hist_prev(shell_hist_t *h, const char *current);

/* Down. Returns the next newer entry, then the draft once past the newest,
 * ending the browse. NULL if not browsing. */
const char *shell_hist_next(shell_hist_t *h);

#endif
