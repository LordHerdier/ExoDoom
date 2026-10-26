/*
 * shell_install.c — see shell_install.h (SCRUM-233).
 *
 * Every step mounts, does one thing, and unmounts before the next, so the
 * one-volume-at-a-time rule (docs/filesystem.md §8) holds at every return
 * and the destination's FAT is flushed before the caller releases the disk.
 * Remounting re-reads the resident FAT, which is why the shell hands in a
 * large chunk: the cost is per chunk, not per byte.
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

/* Read `n` bytes at `off` of `path` on `v` into `buf`. A short read means
 * the file shrank under us, which this reports as -EXO_EIO rather than
 * installing a package with a hole in it. */
static int read_chunk(const shell_vol_t *v, const char *path, uint32_t off,
                      uint8_t *buf, uint32_t n) {
    int rc = exofs_mount(v->dev, v->base);
    if (rc != 0) {
        return rc;
    }
    exofs_file_t f;
    rc = exofs_open(path, EXOFS_O_READ, &f);
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
    exofs_unmount();
    return rc;
}

/* Write `n` bytes of `buf` at `off` of `path` on `v`. The close is checked:
 * it is what writes the size and chain head back to the directory entry. */
static int write_chunk(const shell_vol_t *v, const char *path, uint32_t off,
                       const uint8_t *buf, uint32_t n) {
    int rc = exofs_mount(v->dev, v->base);
    if (rc != 0) {
        return rc;
    }
    exofs_file_t f;
    rc = exofs_open(path, EXOFS_O_WRITE, &f);
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
    exofs_unmount();
    return rc;
}

int shell_copy_between(const shell_vol_t *src, const char *spath,
                       const shell_vol_t *dst, const char *dpath,
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
    int rc = exofs_mount(src->dev, src->base);
    if (rc != 0) {
        return rc;
    }
    rc = exofs_stat(spath, &st);
    exofs_unmount();
    if (rc != 0) {
        return rc;
    }
    if (st.attributes & EXOFS_ATTR_DIRECTORY) {
        return -EXO_EISDIR;
    }

    /* Create or empty the destination file; each chunk then reopens it. */
    rc = exofs_mount(dst->dev, dst->base);
    if (rc != 0) {
        return rc;
    }
    exofs_file_t f;
    rc = exofs_open(dpath, EXOFS_O_WRITE | EXOFS_O_CREATE | EXOFS_O_TRUNC, &f);
    int created = (rc == 0);
    if (created) {
        rc = exofs_close(&f);
    }
    exofs_unmount();

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
        if (created && exofs_mount(dst->dev, dst->base) == 0) {
            exofs_unlink(dpath);
            exofs_unmount();
        }
        return rc;
    }
    if (copied != NULL) {
        *copied = off;
    }
    return 0;
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

    int rc = exofs_mount(src->dev, src->base);
    if (rc != 0) {
        return rc;
    }
    exofs_dir_t dir;
    rc = exofs_opendir("/", &dir);
    if (rc != 0) {
        exofs_unmount();
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
    exofs_unmount();
    return rc;
}
