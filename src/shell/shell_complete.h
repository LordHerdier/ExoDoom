/*
 * shell_complete.h — Tab completion for the shell (SCRUM-274).
 *
 * Pure string logic -- no console, no syscalls, no libc -- so the same TU
 * links into the ring-3 shell blob and, under TESTING=1, into the kernel for
 * tests/kernel/test_shell_complete_k.c (docker/scripts/build.sh), the
 * arrangement shell_hist.c and shell_mount.c already use.
 *
 * Three steps, the first and last pure, the middle owned by shell_main.c:
 *   1. shell_comp_classify() says what the word at the end of the line is
 *      (a command, a program, an install-disk file, a path) and which part
 *      of it is the prefix to match.
 *   2. The shell fills a shell_comp_set_t with shell_comp_add(), one call per
 *      name it can see (command table, directory listing). Only names that
 *      match the prefix are kept.
 *   3. shell_comp_apply() turns the set into the text to append to the line.
 *
 * Completion is at the end of the line only. Matching is case-insensitive
 * (shell_ci_prefix()); the text appended uses the candidate's own case.
 */
#ifndef EXODOOM_SHELL_COMPLETE_H
#define EXODOOM_SHELL_COMPLETE_H

#include <stddef.h>

/* Candidates held. More matches than this set the overflow flag and the
 * extras are dropped; what is held is still correct. */
#define SHELL_COMP_MAX 24u

/* Bytes per candidate name, NUL included. A longer name (ExoFS allows 255)
 * is skipped rather than truncated: a truncated name would complete to a
 * file that does not exist. */
#define SHELL_COMP_NAME_LEN 32u

typedef enum {
    SHELL_COMP_NONE,     /* nothing to complete (e.g. after `echo`)        */
    SHELL_COMP_COMMAND,  /* first word: the command table                  */
    SHELL_COMP_PROGRAM,  /* after `run`: installed *.exo programs          */
    SHELL_COMP_INSTALL,  /* after `install`: files on the install disk     */
    SHELL_COMP_PATH,     /* after cat/ls/rm/cp/mv/mkdir/touch: any entry   */
    SHELL_COMP_DIR       /* after `cd`: directories only                   */
} shell_comp_kind_t;

typedef struct {
    shell_comp_kind_t kind;
    size_t dir_off;      /* start of the word being completed              */
    size_t dir_len;      /* PATH/DIR: bytes up to and including the last
                          * '/' of the word (0 if none); else 0            */
    size_t leaf_off;     /* start of the prefix to match (dir_off + dir_len) */
    size_t leaf_len;     /* bytes of that prefix, up to the end of the line */
} shell_comp_ctx_t;

typedef struct {
    char     name[SHELL_COMP_MAX][SHELL_COMP_NAME_LEN];
    unsigned char is_dir[SHELL_COMP_MAX];
    unsigned count;
    int      overflow;   /* non-zero once a match was dropped              */
} shell_comp_set_t;

/* Classify the NUL-terminated `line` of `len` bytes. Always fills `ctx`. */
void shell_comp_classify(const char *line, size_t len, shell_comp_ctx_t *ctx);

void shell_comp_init(shell_comp_set_t *set);

/* Offer `name`. Kept if it starts with the first `plen` bytes of `prefix`
 * (case-insensitively), is not already held and fits SHELL_COMP_NAME_LEN.
 * Returns 1 if kept. */
int shell_comp_add(shell_comp_set_t *set, const char *name, int is_dir,
                   const char *prefix, size_t plen);

/* Non-zero if `name` is an installable program: a file name ending in ".exo"
 * with at least one character before it. The same rule `programs` lists by. */
int shell_comp_is_program(const char *name);

/* Write into `out` (capacity `out_len`, NUL-terminated) the text to append
 * to a line whose prefix is `plen` bytes long, and return its length:
 *   - no candidates: nothing (0);
 *   - one: the rest of the name, then '/' for a directory or ' ' otherwise;
 *   - several: their longest common prefix beyond `plen` (possibly 0).
 * The set must have been filled against that same `plen`-byte prefix. */
size_t shell_comp_apply(const shell_comp_set_t *set, size_t plen, char *out,
                        size_t out_len);

#endif
