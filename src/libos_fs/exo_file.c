/*
 * exo_file.c — descriptor table over ExoFS (SCRUM-44). See exo_file.h.
 *
 * Deliberately thin: a descriptor is an index into a fixed table of
 * exofs_file_t handles, and every operation is the matching exofs_* call on
 * that slot. All the file semantics — chains, sizes, holes, durability —
 * are ExoFS's, and repeating any of them here would only give them a
 * second place to go wrong.
 */

#include "exo_file.h"

#include "exo_errno.h"
#include "exofs_internal.h"

#include <stddef.h>

/* A slot is free exactly when its handle's `open` is 0, which exofs_close()
 * clears and a zeroed static starts as — no separate in-use flag to keep in
 * step with it. */
static exofs_file_t g_fds[EXO_FILE_MAX_FDS];

/* The volume path-taking calls resolve against, and the mount it was set
 * from. The generation makes a default that outlived its unmount read as
 * "none" rather than silently naming whatever volume took the slot next. */
static exofs_vol_t *g_default_vol;
static uint32_t     g_default_gen;

void exo_file_set_volume(exofs_vol_t *v)
{
    g_default_vol = v;
    g_default_gen = v != NULL ? v->gen : 0;
}

exofs_vol_t *exo_file_volume(void)
{
    return exofs_vol_live(g_default_vol, g_default_gen) ? g_default_vol : NULL;
}

static exofs_file_t *fd_handle(int fd)
{
    if (fd < 0 || fd >= EXO_FILE_MAX_FDS) return NULL;
    if (!g_fds[fd].open) return NULL;
    return &g_fds[fd];
}

int exo_file_open(const char *path, uint32_t flags)
{
    for (int fd = 0; fd < EXO_FILE_MAX_FDS; fd++) {
        if (g_fds[fd].open) continue;

        /* exofs_open() only marks the handle open on success, so a failure
         * leaves the slot free with no cleanup here. */
        int rc = exofs_open(exo_file_volume(), path, flags, &g_fds[fd]);
        if (rc < 0) return rc;
        return fd;
    }
    return -EXO_EMFILE;
}

int exo_file_close(int fd)
{
    exofs_file_t *f = fd_handle(fd);
    if (f == NULL) return -EXO_EBADF;
    return exofs_close(f);
}

int64_t exo_file_read(int fd, void *buf, uint32_t n)
{
    exofs_file_t *f = fd_handle(fd);
    if (f == NULL) return -EXO_EBADF;
    return exofs_read(f, buf, n);
}

int64_t exo_file_write(int fd, const void *buf, uint32_t n)
{
    exofs_file_t *f = fd_handle(fd);
    if (f == NULL) return -EXO_EBADF;
    return exofs_write(f, buf, n);
}

int64_t exo_file_seek(int fd, int64_t off, uint32_t whence)
{
    exofs_file_t *f = fd_handle(fd);
    if (f == NULL) return -EXO_EBADF;
    return exofs_seek(f, off, whence);
}

int exo_file_stat(const char *path, uint32_t *size_out)
{
    if (size_out == NULL) return -EXO_EINVAL;

    exofs_stat_t st;
    int rc = exofs_stat(exo_file_volume(), path, &st);
    if (rc < 0) return rc;

    *size_out = st.size;
    return 0;
}

int exo_file_close_all(void)
{
    int first = 0;
    for (int fd = 0; fd < EXO_FILE_MAX_FDS; fd++) {
        if (!g_fds[fd].open) continue;
        int rc = exofs_close(&g_fds[fd]);
        if (rc < 0 && first == 0) first = rc;
    }
    return first;
}
