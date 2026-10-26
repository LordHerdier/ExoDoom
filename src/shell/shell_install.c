/*
 * shell_install.c — see shell_install.h (SCRUM-233).
 *
 * A copy holds the source and the destination mounted together for its whole
 * length (SCRUM-265), so the FAT of each is read once, not once per chunk.
 * Everything is unmounted before returning, which is what flushes the
 * destination's FAT before the caller releases the disk.
 */
#include "shell/shell_install.h"

#include "exo_errno.h"
#include "libos_fs/exofs.h"

int shell_install_resolve(const char *name, char *out, size_t out_len) {
    if (name == NULL || name[0] == '\0' || out_len == 0) {
        return -EXO_EINVAL;
    }
    size_t n = 0;
    if (name[0] != '/') {
        out[n++] = '/';
    }
    int has_dot = 0;
    for (size_t k = 0; name[k] != '\0'; k++) {
        if (n + 1 >= out_len) {
            return -EXO_EINVAL;
        }
        if (name[k] == '/') {
            has_dot = 0;             /* only the last component counts */
        } else if (name[k] == '.') {
            has_dot = 1;
        }
        out[n++] = name[k];
    }
    if (n == 1 && out[0] == '/') {
        return -EXO_EINVAL;          /* bare "/" names no file */
    }
    if (!has_dot) {
        static const char ext[] = ".exo";
        for (size_t k = 0; ext[k] != '\0'; k++) {
            if (n + 1 >= out_len) {
                return -EXO_EINVAL;
            }
            out[n++] = ext[k];
        }
    }
    out[n] = '\0';
    return 0;
}

static char run_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

/* Rank 1: `want` has no ".exo" suffix and `entry` is `want` + ".exo",
 * ignoring case. */
static int shell_run_rank_exo(const char *want, size_t wn, const char *entry) {
    static const char ext[] = ".exo";
    if (wn >= 4) {
        size_t k = 0;
        while (k < 4 && run_lower(want[wn - 4 + k]) == ext[k]) {
            k++;
        }
        if (k == 4) {
            return 0;                /* already has the suffix */
        }
    }
    for (size_t i = 0; i < wn; i++) {
        if (run_lower(want[i]) != run_lower(entry[i])) {
            return 0;
        }
        if (entry[i] == '\0') {
            return 0;
        }
    }
    for (size_t k = 0; k < 4; k++) {
        if (run_lower(entry[wn + k]) != ext[k]) {
            return 0;
        }
    }
    return entry[wn + 4] == '\0' ? 1 : 0;
}

int shell_run_match_rank(const char *want, const char *entry) {
    if (want == NULL || entry == NULL || want[0] == '\0') {
        return 0;
    }
    size_t wn = 0;
    while (want[wn] != '\0') {
        wn++;
    }
    int exact = 1;
    size_t i = 0;
    for (; want[i] != '\0' && entry[i] != '\0'; i++) {
        if (run_lower(want[i]) != run_lower(entry[i])) {
            return shell_run_rank_exo(want, wn, entry);
        }
        if (want[i] != entry[i]) {
            exact = 0;
        }
    }
    if (want[i] == '\0' && entry[i] == '\0') {
        return exact ? 3 : 2;
    }
    return shell_run_rank_exo(want, wn, entry);
}

/* Read `n` bytes at `off` of `path` on `v` into `buf`. A short read means
 * the file shrank under us, which this reports as -EXO_EIO rather than
 * installing a package with a hole in it. */
static int read_chunk(exofs_vol_t *v, const char *path, uint32_t off,
                      uint8_t *buf, uint32_t n) {
    exofs_file_t f;
    int rc = exofs_open(v, path, EXOFS_O_READ, &f);
    if (rc == 0) {
        int64_t pos = exofs_seek(&f, off, EXOFS_SEEK_SET);
        uint32_t have = 0;
        if (pos < 0) {
            rc = (int)pos;
        }
        while (rc == 0 && have < n) {
            int64_t got = exofs_read(&f, buf + have, n - have);
            if (got <= 0) {
                rc = got < 0 ? (int)got : -EXO_EIO;
                break;
            }
            have += (uint32_t)got;
        }
        exofs_close(&f);
    }
    return rc;
}

/* Write `n` bytes of `buf` at `off` of `path` on `v`. The close is checked:
 * it is what writes the size and chain head back to the directory entry. */
static int write_chunk(exofs_vol_t *v, const char *path, uint32_t off,
                       const uint8_t *buf, uint32_t n) {
    exofs_file_t f;
    int rc = exofs_open(v, path, EXOFS_O_WRITE, &f);
    if (rc == 0) {
        int64_t pos = exofs_seek(&f, off, EXOFS_SEEK_SET);
        if (pos < 0) {
            rc = (int)pos;
        } else {
            int64_t wrote = exofs_write(&f, buf, n);
            if (wrote < 0) {
                rc = (int)wrote;
            } else if ((uint64_t)wrote != n) {
                rc = -EXO_ENOSPC;
            }
        }
        int crc = exofs_close(&f);
        if (rc == 0) {
            rc = crc;
        }
    }
    return rc;
}

int shell_copy_vols(exofs_vol_t *src, const char *spath,
                    exofs_vol_t *dst, const char *dpath,
                    uint8_t *buf, uint32_t chunk, uint32_t *copied) {
    if (copied != NULL) {
        *copied = 0;
    }
    if (buf == NULL || chunk == 0) {
        return -EXO_EINVAL;
    }

    /* Look before touching the destination: a typo must not truncate an
     * installed package of that name. */
    exofs_stat_t st;
    int rc = exofs_stat(src, spath, &st);
    if (rc != 0) {
        return rc;
    }
    if (st.attributes & EXOFS_ATTR_DIRECTORY) {
        return -EXO_EISDIR;
    }

    /* Create or empty the destination file; each chunk then reopens it. */
    exofs_file_t f;
    rc = exofs_open(dst, dpath, EXOFS_O_WRITE | EXOFS_O_CREATE | EXOFS_O_TRUNC,
                    &f);
    int created = (rc == 0);
    if (created) {
        rc = exofs_close(&f);
    }

    uint32_t off = 0;
    while (rc == 0 && off < st.size) {
        uint32_t n = st.size - off;
        if (n > chunk) {
            n = chunk;
        }
        rc = read_chunk(src, spath, off, buf, n);
        if (rc == 0) {
            rc = write_chunk(dst, dpath, off, buf, n);
        }
        if (rc == 0) {
            off += n;
        }
    }

    if (rc != 0) {
        if (created) {
            exofs_unlink(dst, dpath);
        }
        return rc;
    }
    if (copied != NULL) {
        *copied = off;
    }
    return 0;
}

int shell_copy_between(const shell_vol_t *src, const char *spath,
                       const shell_vol_t *dst, const char *dpath,
                       uint8_t *buf, uint32_t chunk, uint32_t *copied) {
    if (copied != NULL) {
        *copied = 0;
    }

    exofs_vol_t *sv = NULL;
    int rc = exofs_mount(src->dev, src->base, &sv);
    if (rc != 0) {
        return rc;
    }

    /* The same volume on both sides is one mount: ExoFS refuses a second
     * resident copy of a volume's FAT. */
    exofs_vol_t *dv = sv;
    if (dst->dev != src->dev || dst->base != src->base) {
        rc = exofs_mount(dst->dev, dst->base, &dv);
        if (rc != 0) {
            exofs_unmount(sv);
            return rc;
        }
    }

    rc = shell_copy_vols(sv, spath, dv, dpath, buf, chunk, copied);

    if (dv != sv) {
        exofs_unmount(dv);
    }
    exofs_unmount(sv);
    return rc;
}

int shell_install_copy(const shell_vol_t *src, const shell_vol_t *dst,
                       const char *path, uint8_t *buf, uint32_t chunk,
                       uint32_t *copied) {
    return shell_copy_between(src, path, dst, path, buf, chunk, copied);
}

int shell_install_is_glob(const char *path) {
    for (size_t k = 0; path[k] != '\0'; k++) {
        if (path[k] == '*') {
            return 1;
        }
    }
    return 0;
}

/* Greedy with one backtrack point: on a mismatch, let the last '*' swallow
 * one more character and retry. Linear for one '*', and never recursive. */
int shell_install_glob_match(const char *pattern, const char *name) {
    const char *p = pattern, *n = name;
    const char *star = NULL, *resume = NULL;
    while (*n != '\0') {
        if (*p == '*') {
            star = p++;
            resume = n;
        } else if (*p == *n) {
            p++;
            n++;
        } else if (star != NULL) {
            p = star + 1;
            n = ++resume;
        } else {
            return 0;
        }
    }
    while (*p == '*') {
        p++;
    }
    return *p == '\0';
}

int shell_install_matches(const shell_vol_t *src, const char *pattern,
                          char *names, uint32_t stride, uint32_t max,
                          uint32_t *count) {
    *count = 0;
    const char *pat = pattern[0] == '/' ? pattern + 1 : pattern;
    for (size_t k = 0; pat[k] != '\0'; k++) {
        if (pat[k] == '/') {
            return -EXO_EINVAL;
        }
    }

    exofs_vol_t *v = NULL;
    int rc = exofs_mount(src->dev, src->base, &v);
    if (rc != 0) {
        return rc;
    }
    exofs_dir_t dir;
    rc = exofs_opendir(v, "/", &dir);
    if (rc != 0) {
        exofs_unmount(v);
        return rc;
    }
    for (;;) {
        exofs_dirinfo_t info;
        int more = exofs_readdir(&dir, &info);
        if (more < 0) {
            rc = more;
            break;
        }
        if (more == 0) {
            break;
        }
        if ((info.attributes & EXOFS_ATTR_DIRECTORY) ||
            !shell_install_glob_match(pat, info.name)) {
            continue;
        }
        if (*count == max) {
            rc = -EXO_ENOSPC;
            break;
        }
        char *out = names + (size_t)*count * stride;
        size_t n = 0;
        out[n++] = '/';
        for (size_t k = 0; info.name[k] != '\0'; k++) {
            if (n + 1 >= stride) {
                rc = -EXO_EINVAL;
                break;
            }
            out[n++] = info.name[k];
        }
        if (rc != 0) {
            break;
        }
        out[n] = '\0';
        (*count)++;
    }
    exofs_closedir(&dir);
    exofs_unmount(v);
    return rc;
}
