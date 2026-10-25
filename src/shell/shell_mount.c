/*
 * shell_mount.c — see shell_mount.h (SCRUM-266).
 */
#include "shell/shell_mount.h"

#include "exo_errno.h"
#include "libos_fs/exofs_layout.h"

static size_t path_len(const char *s) {
    size_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

int shell_mount_normalize(const char *in, char *out, size_t out_len) {
    if (in == NULL || out == NULL || in[0] != '/' || out_len < 2) {
        return -EXO_EINVAL;
    }

    /* `n` is the length of the result so far; every component written is
     * preceded by its '/', so ".." pops back to the last '/'. */
    size_t n = 0;
    size_t i = 0;
    while (in[i] != '\0') {
        while (in[i] == '/') {
            i++;
        }
        size_t start = i;
        while (in[i] != '\0' && in[i] != '/') {
            i++;
        }
        size_t len = i - start;
        if (len == 0) {
            break;
        }
        if (len == 1 && in[start] == '.') {
            continue;
        }
        if (len == 2 && in[start] == '.' && in[start + 1] == '.') {
            while (n > 0 && out[n - 1] != '/') {
                n--;
            }
            if (n > 0) {
                n--;                 /* drop the '/' too */
            }
            continue;
        }
        if (n + 1 + len + 1 > out_len) {
            return -EXO_EINVAL;
        }
        out[n++] = '/';
        for (size_t k = 0; k < len; k++) {
            out[n++] = in[start + k];
        }
    }
    if (n == 0) {
        out[n++] = '/';
    }
    out[n] = '\0';
    return 0;
}

int shell_mount_path_within(const char *path, const char *dir) {
    if (dir[0] == '/' && dir[1] == '\0') {
        return path[0] == '/';
    }
    size_t n = path_len(dir);
    for (size_t k = 0; k < n; k++) {
        if (path[k] != dir[k]) {
            return 0;
        }
    }
    return path[n] == '\0' || path[n] == '/';
}

void shell_mount_init(shell_mount_table_t *t) {
    for (size_t i = 0; i < SHELL_MOUNT_MAX; i++) {
        t->e[i].used = 0;
        t->e[i].point[0] = '\0';
    }
}

static int is_root(const char *p) {
    return p[0] == '/' && p[1] == '\0';
}

int shell_mount_find(const shell_mount_table_t *t, const char *path) {
    for (size_t i = 0; i < SHELL_MOUNT_MAX; i++) {
        if (!t->e[i].used) {
            continue;
        }
        const char *a = t->e[i].point;
        size_t k = 0;
        while (a[k] != '\0' && a[k] == path[k]) {
            k++;
        }
        if (a[k] == '\0' && path[k] == '\0') {
            return (int)i;
        }
    }
    return -1;
}

int shell_mount_has_below(const shell_mount_table_t *t, const char *path) {
    for (size_t i = 0; i < SHELL_MOUNT_MAX; i++) {
        if (t->e[i].used &&
            shell_mount_find(t, path) != (int)i &&
            shell_mount_path_within(t->e[i].point, path)) {
            return 1;
        }
    }
    return 0;
}

int shell_mount_add(shell_mount_table_t *t, const char *point,
                    const shell_volref_t *vol) {
    if (point == NULL || point[0] != '/' || is_root(point) ||
        path_len(point) >= SHELL_MOUNT_PATH_LEN) {
        return -EXO_EINVAL;
    }
    if (shell_mount_find(t, point) >= 0) {
        return -EXO_EBUSY;
    }
    for (size_t i = 0; i < SHELL_MOUNT_MAX; i++) {
        if (!t->e[i].used) {
            size_t k = 0;
            for (; point[k] != '\0'; k++) {
                t->e[i].point[k] = point[k];
            }
            t->e[i].point[k] = '\0';
            t->e[i].vol = *vol;
            t->e[i].used = 1;
            return 0;
        }
    }
    return -EXO_ENOSPC;
}

int shell_mount_remove(shell_mount_table_t *t, const char *point,
                       const char *cwd) {
    int idx = shell_mount_find(t, point);
    if (idx < 0) {
        return -EXO_EINVAL;
    }
    if (shell_mount_path_within(cwd, point) ||
        shell_mount_has_below(t, point)) {
        return -EXO_EBUSY;
    }
    t->e[idx].used = 0;
    t->e[idx].point[0] = '\0';
    return 0;
}

int shell_mount_route(const shell_mount_table_t *t, const shell_volref_t *root,
                      const char *path, shell_volref_t *vol, char *inner,
                      size_t inner_len, int *mount_idx) {
    if (path == NULL || path[0] != '/' || inner_len < 2) {
        return -EXO_EINVAL;
    }

    int best = -1;
    size_t best_len = 0;
    for (size_t i = 0; i < SHELL_MOUNT_MAX; i++) {
        if (!t->e[i].used || !shell_mount_path_within(path, t->e[i].point)) {
            continue;
        }
        size_t len = path_len(t->e[i].point);
        if (best < 0 || len > best_len) {
            best = (int)i;
            best_len = len;
        }
    }

    const char *rest = path + best_len;   /* best_len == 0 for the root */
    if (best < 0) {
        *vol = *root;
    } else {
        *vol = t->e[best].vol;
    }
    if (rest[0] == '\0') {
        inner[0] = '/';
        inner[1] = '\0';
    } else {
        size_t n = path_len(rest);
        if (n + 1 > inner_len) {
            return -EXO_EINVAL;
        }
        for (size_t k = 0; k <= n; k++) {
            inner[k] = rest[k];
        }
    }
    if (mount_idx != NULL) {
        *mount_idx = best;
    }
    return 0;
}

int shell_mount_sb_valid(const void *sector) {
    const uint8_t *b = sector;
    if (b == NULL) {
        return 0;
    }
    /* Field offsets are those of exofs_super_t (little-endian on disk). */
    uint32_t magic = (uint32_t)b[0] | (uint32_t)b[1] << 8 |
                     (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    uint32_t version = (uint32_t)b[4] | (uint32_t)b[5] << 8;
    uint32_t block = (uint32_t)b[6] | (uint32_t)b[7] << 8;
    return magic == EXOFS_MAGIC && version == EXOFS_VERSION &&
           block == EXOFS_BLOCK_SIZE;
}

/* Parse a run of decimal digits at *p into *out, advancing *p. At least one
 * digit and at most four (see shell_mount_parse_volname()). */
static int parse_digits(const char **p, uint32_t *out) {
    const char *s = *p;
    uint32_t v = 0;
    size_t n = 0;
    while (s[n] >= '0' && s[n] <= '9') {
        if (n >= 4) {
            return -EXO_EINVAL;
        }
        v = v * 10u + (uint32_t)(s[n] - '0');
        n++;
    }
    if (n == 0) {
        return -EXO_EINVAL;
    }
    *out = v;
    *p = s + n;
    return 0;
}

static int take_word(const char **p, const char *word) {
    const char *s = *p;
    for (size_t i = 0; word[i] != '\0'; i++) {
        if (s[i] != word[i]) {
            return 0;
        }
    }
    *p = s + path_len(word);
    return 1;
}

int shell_mount_parse_volname(const char *s, uint32_t *disk, uint32_t *vol) {
    if (s == NULL) {
        return -EXO_EINVAL;
    }
    uint32_t d, v;
    if (!take_word(&s, "disk") || parse_digits(&s, &d) != 0 ||
        !take_word(&s, ":vol") || parse_digits(&s, &v) != 0 ||
        *s != '\0' || v < 1) {
        return -EXO_EINVAL;
    }
    *disk = d;
    *vol = v;
    return 0;
}
