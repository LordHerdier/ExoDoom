/*
 * shell_hist.c — the shell's command history (SCRUM-273). See shell_hist.h.
 */

#include "shell/shell_hist.h"

static void copy_line(char *dst, const char *src)
{
    size_t i = 0;

    while (i < SHELL_HIST_LEN - 1 && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int same_line(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* The entry `back` places behind the newest (0 = newest). */
static const char *entry(const shell_hist_t *h, unsigned back)
{
    return h->ent[(h->next + SHELL_HIST_MAX - 1 - back) % SHELL_HIST_MAX];
}

void shell_hist_init(shell_hist_t *h)
{
    h->count = 0;
    h->next = 0;
    h->pos = 0;
    h->browsing = 0;
    h->draft[0] = '\0';
}

void shell_hist_push(shell_hist_t *h, const char *line)
{
    h->browsing = 0;
    if (line[0] == '\0') {
        return;
    }
    if (h->count > 0) {
        char cut[SHELL_HIST_LEN];

        /* Compare what would be stored, so a line cut at the limit still
         * collapses against its own stored copy. */
        copy_line(cut, line);
        if (same_line(cut, entry(h, 0))) {
            return;
        }
    }
    copy_line(h->ent[h->next], line);
    h->next = (h->next + 1) % SHELL_HIST_MAX;
    if (h->count < SHELL_HIST_MAX) {
        h->count++;
    }
}

const char *shell_hist_prev(shell_hist_t *h, const char *current)
{
    if (h->count == 0) {
        return NULL;
    }
    if (!h->browsing) {
        copy_line(h->draft, current);
        h->browsing = 1;
        h->pos = 0;
    } else if (h->pos + 1 < h->count) {
        h->pos++;
    }
    return entry(h, h->pos);
}

const char *shell_hist_next(shell_hist_t *h)
{
    if (!h->browsing) {
        return NULL;
    }
    if (h->pos == 0) {
        h->browsing = 0;
        return h->draft;
    }
    h->pos--;
    return entry(h, h->pos);
}
