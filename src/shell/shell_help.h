/*
 * shell_help.h — command-help metadata and matching for the shell LibOS
 * (SCRUM-263).
 *
 * The command table itself lives in src/shell/shell_main.c, next to the
 * handlers it points at; this is only the part of it that `help`, `<cmd> ?`
 * and usage errors read, plus the pure string helpers they need. Nothing
 * here touches the framebuffer console or makes a syscall, so the same TU
 * links into the ring-3 shell blob and, under TESTING=1, into the kernel for
 * tests/kernel/test_shell_help_k.c (docker/scripts/build.sh).
 */
#ifndef EXODOOM_SHELL_HELP_H
#define EXODOOM_SHELL_HELP_H

#include <stddef.h>

typedef enum {
    SHELL_GRP_FS,
    SHELL_GRP_DISK,
    SHELL_GRP_APPS,
    SHELL_GRP_SYSTEM,
    SHELL_GRP_COUNT
} shell_group_t;

typedef struct {
    const char *name;    /* lowercase; matched case-insensitively */
    shell_group_t group;
    const char *summary; /* one line, no newline -- the `help all` row */
    const char *usage;   /* full `<cmd> ?` text, newline-terminated lines */
} shell_help_entry_t;

typedef enum {
    SHELL_HELP_Q_INDEX,  /* bare `help` */
    SHELL_HELP_Q_ALL,    /* `help all` */
    SHELL_HELP_Q_GROUP,  /* `help fs` etc.; .group is set */
    SHELL_HELP_Q_WORD,   /* `help cat`: a command name if one matches, else a prefix */
    SHELL_HELP_Q_PREFIX  /* `help c*`: always a prefix, never an exact name */
} shell_help_qkind_t;

typedef struct {
    shell_help_qkind_t kind;
    shell_group_t group;
    const char *word;    /* points into the parsed argument, not NUL-terminated */
    size_t word_len;     /* trailing '*' already stripped */
} shell_help_query_t;

/* Width of the NAME and GROUP columns in a `help all` row. */
#define SHELL_HELP_NAME_COL  10
#define SHELL_HELP_GROUP_COL 8

/* 1 if the first `len` chars of `tok` equal all of `name`, ignoring ASCII
 * case. `tok` need not be NUL-terminated. */
int shell_ci_token_eq(const char *tok, size_t len, const char *name);

/* 1 if `name` starts with the first `len` chars of `prefix`, ignoring ASCII
 * case. len == 0 matches everything. */
int shell_ci_prefix(const char *name, const char *prefix, size_t len);

/* "FS", "DISK", ... ; "?" for an out-of-range value. */
const char *shell_group_name(shell_group_t g);

/* Case-insensitive group-name lookup. Returns 1 and sets *out on a match. */
int shell_group_parse(const char *tok, size_t len, shell_group_t *out);

/* Parse what follows `help` (leading spaces already skipped by the caller,
 * trailing spaces ignored here). Only the first word counts. */
shell_help_query_t shell_help_query_parse(const char *arg);

/* Does `e` belong in the listing for `q`? INDEX matches nothing; WORD and
 * PREFIX are both prefix matches here -- an exact-name WORD is resolved by
 * the caller before it ever lists anything. */
int shell_help_entry_matches(const shell_help_entry_t *e,
                             const shell_help_query_t *q);

/* Format one `help all` row -- "CAT       FS      Print a file...\n" --
 * into buf. Always NUL-terminates when len > 0, truncating (and dropping
 * the newline) if buf is too small. Returns the length written, excluding
 * the NUL. */
size_t shell_help_format_row(const shell_help_entry_t *e, char *buf,
                             size_t len);

#endif /* EXODOOM_SHELL_HELP_H */
