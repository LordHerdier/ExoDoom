/*
 * shell_help.c — see shell_help.h (SCRUM-263).
 *
 * Deliberately self-contained: no libc calls, not even the shim's tolower(),
 * so this TU means the same thing compiled as ring-3 shell code and as the
 * TESTING kernel's copy of it.
 */
#include "shell/shell_help.h"

static char shell_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

static char shell_upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - ('a' - 'A')) : c;
}

int shell_ci_token_eq(const char *tok, size_t len, const char *name) {
    size_t i = 0;
    for (; i < len; i++) {
        if (name[i] == '\0' || shell_lower(tok[i]) != shell_lower(name[i])) {
            return 0;
        }
    }
    return name[i] == '\0';
}

int shell_ci_prefix(const char *name, const char *prefix, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (name[i] == '\0' || shell_lower(name[i]) != shell_lower(prefix[i])) {
            return 0;
        }
    }
    return 1;
}

static const char *const shell_group_names[SHELL_GRP_COUNT] = {
    [SHELL_GRP_FS] = "FS",
    [SHELL_GRP_DISK] = "DISK",
    [SHELL_GRP_APPS] = "APPS",
    [SHELL_GRP_SYSTEM] = "SYSTEM",
};

const char *shell_group_name(shell_group_t g) {
    if ((unsigned)g >= SHELL_GRP_COUNT) {
        return "?";
    }
    return shell_group_names[g];
}

int shell_group_parse(const char *tok, size_t len, shell_group_t *out) {
    for (int g = 0; g < SHELL_GRP_COUNT; g++) {
        if (shell_ci_token_eq(tok, len, shell_group_names[g])) {
            *out = (shell_group_t)g;
            return 1;
        }
    }
    return 0;
}

shell_help_query_t shell_help_query_parse(const char *arg) {
    shell_help_query_t q;
    q.kind = SHELL_HELP_Q_INDEX;
    q.group = SHELL_GRP_FS;
    q.word = arg;
    q.word_len = 0;

    while (*arg == ' ') {
        arg++;
    }
    size_t len = 0;
    while (arg[len] != '\0' && arg[len] != ' ') {
        len++;
    }
    q.word = arg;
    if (len == 0) {
        return q;
    }

    if (arg[len - 1] == '*') {
        q.kind = SHELL_HELP_Q_PREFIX;
        q.word_len = len - 1;
        return q;
    }
    q.word_len = len;
    if (shell_ci_token_eq(arg, len, "all")) {
        q.kind = SHELL_HELP_Q_ALL;
    } else if (shell_group_parse(arg, len, &q.group)) {
        q.kind = SHELL_HELP_Q_GROUP;
    } else {
        q.kind = SHELL_HELP_Q_WORD;
    }
    return q;
}

int shell_help_entry_matches(const shell_help_entry_t *e,
                             const shell_help_query_t *q) {
    switch (q->kind) {
    case SHELL_HELP_Q_ALL:
        return 1;
    case SHELL_HELP_Q_GROUP:
        return e->group == q->group;
    case SHELL_HELP_Q_WORD:
    case SHELL_HELP_Q_PREFIX:
        return shell_ci_prefix(e->name, q->word, q->word_len);
    default:
        return 0;
    }
}

/* Append `c` if there is room for it plus the final NUL. */
static void shell_row_put(char *buf, size_t len, size_t *pos, char c) {
    if (*pos + 1 < len) {
        buf[(*pos)++] = c;
    }
}

size_t shell_help_format_row(const shell_help_entry_t *e, char *buf,
                             size_t len) {
    size_t pos = 0;
    if (len == 0) {
        return 0;
    }

    size_t col = 0;
    for (const char *s = e->name; *s != '\0'; s++, col++) {
        shell_row_put(buf, len, &pos, shell_upper(*s));
    }
    /* At least one space even if a name ever outgrows its column. */
    do {
        shell_row_put(buf, len, &pos, ' ');
    } while (++col < SHELL_HELP_NAME_COL);

    col = 0;
    for (const char *s = shell_group_name(e->group); *s != '\0'; s++, col++) {
        shell_row_put(buf, len, &pos, *s);
    }
    do {
        shell_row_put(buf, len, &pos, ' ');
    } while (++col < SHELL_HELP_GROUP_COL);

    for (const char *s = e->summary; *s != '\0'; s++) {
        shell_row_put(buf, len, &pos, *s);
    }
    shell_row_put(buf, len, &pos, '\n');

    buf[pos] = '\0';
    return pos;
}
