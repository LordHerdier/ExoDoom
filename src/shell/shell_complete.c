/*
 * shell_complete.c — Tab completion for the shell (SCRUM-274). See
 * shell_complete.h.
 */

#include "shell/shell_complete.h"
#include "shell/shell_help.h"

static size_t str_len(const char *s)
{
    size_t n = 0;

    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* Does the verb line[0..vlen) name `word` (case-insensitively)? */
static int verb_is(const char *line, size_t vlen, const char *word)
{
    return str_len(word) == vlen && shell_ci_prefix(word, line, vlen);
}

void shell_comp_classify(const char *line, size_t len, shell_comp_ctx_t *ctx)
{
    size_t word = len;

    /* The word being completed starts after the last space. */
    while (word > 0 && line[word - 1] != ' ') {
        word--;
    }

    ctx->kind = SHELL_COMP_NONE;
    ctx->dir_off = word;
    ctx->dir_len = 0;
    ctx->leaf_off = word;
    ctx->leaf_len = len - word;

    /* The verb is the first word, past any leading spaces. */
    size_t vstart = 0;
    while (vstart < word && line[vstart] == ' ') {
        vstart++;
    }
    if (vstart == word) {
        ctx->kind = SHELL_COMP_COMMAND;
        return;
    }
    size_t vend = vstart;
    while (vend < word && line[vend] != ' ') {
        vend++;
    }
    const char *verb = line + vstart;
    size_t vlen = vend - vstart;

    if (verb_is(verb, vlen, "run")) {
        ctx->kind = SHELL_COMP_PROGRAM;
        return;
    }
    if (verb_is(verb, vlen, "install")) {
        ctx->kind = SHELL_COMP_INSTALL;
        return;
    }

    static const char *const path_verbs[] = {
        "cat", "ls", "rm", "cp", "mv", "mkdir", "touch",
    };
    int is_path = 0;
    for (size_t i = 0; i < sizeof(path_verbs) / sizeof(path_verbs[0]); i++) {
        if (verb_is(verb, vlen, path_verbs[i])) {
            is_path = 1;
            break;
        }
    }
    if (is_path) {
        ctx->kind = SHELL_COMP_PATH;
    } else if (verb_is(verb, vlen, "cd")) {
        ctx->kind = SHELL_COMP_DIR;
    } else {
        return;
    }

    /* Split the word at its last '/': the directory to list, then the leaf. */
    size_t slash = len;
    while (slash > word && line[slash - 1] != '/') {
        slash--;
    }
    ctx->dir_len = slash - word;
    ctx->leaf_off = slash;
    ctx->leaf_len = len - slash;
}

void shell_comp_init(shell_comp_set_t *set)
{
    set->count = 0;
    set->overflow = 0;
}

int shell_comp_add(shell_comp_set_t *set, const char *name, int is_dir,
                   const char *prefix, size_t plen)
{
    size_t n = str_len(name);

    if (n == 0 || !shell_ci_prefix(name, prefix, plen)) {
        return 0;
    }
    if (n >= SHELL_COMP_NAME_LEN) {
        set->overflow = 1;
        return 0;
    }
    for (unsigned i = 0; i < set->count; i++) {
        size_t k = 0;
        while (set->name[i][k] != '\0' && set->name[i][k] == name[k]) {
            k++;
        }
        if (set->name[i][k] == name[k]) {
            return 0;
        }
    }
    if (set->count == SHELL_COMP_MAX) {
        set->overflow = 1;
        return 0;
    }
    for (size_t k = 0; k <= n; k++) {
        set->name[set->count][k] = name[k];
    }
    set->is_dir[set->count] = is_dir ? 1 : 0;
    set->count++;
    return 1;
}

int shell_comp_is_program(const char *name)
{
    size_t n = str_len(name);

    return n >= 5 && name[n - 4] == '.' && name[n - 3] == 'e' &&
           name[n - 2] == 'x' && name[n - 1] == 'o';
}

size_t shell_comp_apply(const shell_comp_set_t *set, size_t plen, char *out,
                        size_t out_len)
{
    size_t w = 0;

    if (out_len == 0) {
        return 0;
    }
    out[0] = '\0';
    if (set->count == 0 || set->overflow) {
        return 0;
    }

    const char *first = set->name[0];
    size_t common = str_len(first);

    for (unsigned i = 1; i < set->count; i++) {
        size_t k = 0;
        while (k < common && set->name[i][k] != '\0' &&
               lower(set->name[i][k]) == lower(first[k])) {
            k++;
        }
        common = k;
    }

    for (size_t k = plen; k < common && w + 1 < out_len; k++) {
        out[w++] = first[k];
    }
    if (set->count == 1 && w + 2 <= out_len) {
        out[w++] = set->is_dir[0] ? '/' : ' ';
    }
    out[w] = '\0';
    return w;
}
