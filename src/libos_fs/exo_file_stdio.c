#include "libos_fs/exo_file_stdio.h"

#include "exo_errno.h"
#include "libos_fs/exo_file.h"
#include "libos_fs/exofs.h"
#include "libos_fs/exofs_internal.h"
#include "stdio.h"

#include <stddef.h>
#include <stdint.h>

#define STDIO_PATH_MAX 128

/* The read-through volume (exo_file_stdio_set_fallback), and the mount it was
 * taken from: the generation makes one that outlived its unmount read as
 * "none", the same rule exo_file.c applies to the default volume. */
static exofs_vol_t *g_fallback_vol;
static uint32_t     g_fallback_gen;

/* Turn fopen's path into the absolute one ExoFS wants: a leading "./" is
 * dropped and a relative path is taken from the volume root. "" and "/" both
 * come out as "/". */
static int efs_resolve(const char *path, char *abs, size_t cap)
{
    size_t n = 0;

    while (path[0] == '.' && path[1] == '/')
        path += 2;

    abs[n++] = '/';
    while (*path == '/')
        path++;
    for (; *path != '\0'; path++) {
        if (n + 1 >= cap)
            return -EXO_EINVAL;
        abs[n++] = *path;
    }
    abs[n] = '\0';
    return 0;
}

static int efs_open(const char *path, unsigned flags)
{
    char abs[STDIO_PATH_MAX];
    uint32_t f = 0;
    int rc;

    rc = efs_resolve(path, abs, sizeof abs);
    if (rc < 0)
        return rc;
    if (abs[1] == '\0')
        return -EXO_ENOENT;   /* "" or "/" names no file; not the root dir */

    if (flags & EXO_STDIO_READ)   f |= EXO_FILE_O_READ;
    if (flags & EXO_STDIO_WRITE)  f |= EXO_FILE_O_WRITE;
    if (flags & EXO_STDIO_CREATE) f |= EXO_FILE_O_CREATE;
    if (flags & EXO_STDIO_TRUNC)  f |= EXO_FILE_O_TRUNC;
    if (flags & EXO_STDIO_APPEND) f |= EXO_FILE_O_APPEND;

    int fd = exo_file_open(abs, f);

    /* A read of a file the default volume does not have goes on to the
     * fallback volume, if there is one. Reads only: anything that could write
     * stays on the default volume, so a program's own volume is the only one
     * it can change. Opened on the fallback directly, so the default volume
     * is never touched. */
    if (fd == -EXO_ENOENT &&
        !(flags & (EXO_STDIO_WRITE | EXO_STDIO_CREATE | EXO_STDIO_TRUNC |
                   EXO_STDIO_APPEND)) &&
        exofs_vol_live(g_fallback_vol, g_fallback_gen)) {
        fd = exo_file_open_on(g_fallback_vol, abs, f);
    }
    return fd;
}

static int efs_mkdir(const char *path)
{
    char abs[STDIO_PATH_MAX];
    size_t n;
    int rc;

    rc = efs_resolve(path, abs, sizeof abs);
    if (rc < 0)
        return rc;

    /* "saves/" and "saves" name the same directory. */
    for (n = 0; abs[n] != '\0'; n++)
        ;
    while (n > 1 && abs[n - 1] == '/')
        abs[--n] = '\0';

    if (n == 1)
        return -EXO_EEXIST;   /* the root is always there */

    return exofs_mkdir(exo_file_volume(), abs);
}

static int efs_remove(const char *path)
{
    char abs[STDIO_PATH_MAX];
    int rc;

    rc = efs_resolve(path, abs, sizeof abs);
    if (rc < 0)
        return rc;
    if (abs[1] == '\0')
        return -EXO_EISDIR;   /* the root is not a file to remove */

    return exo_file_remove(abs);
}

static int efs_rename(const char *oldpath, const char *newpath)
{
    char from[STDIO_PATH_MAX];
    char to[STDIO_PATH_MAX];
    int rc;

    rc = efs_resolve(oldpath, from, sizeof from);
    if (rc < 0)
        return rc;
    rc = efs_resolve(newpath, to, sizeof to);
    if (rc < 0)
        return rc;

    return exo_file_rename(from, to);
}

static int64_t efs_read(int handle, void *buf, size_t n)
{
    /* exo_file_read takes a uint32_t; a larger request is just a short read,
     * which fread's loop already handles. */
    uint32_t chunk = n > 0x40000000u ? 0x40000000u : (uint32_t)n;

    return exo_file_read(handle, buf, chunk);
}

static int64_t efs_seek(int handle, int64_t off, int whence)
{
    return exo_file_seek(handle, off, (uint32_t)whence);
}

static int64_t efs_write(int handle, const void *buf, size_t n)
{
    /* Same clamp as efs_read: a larger request is a short write, which
     * stdio.c's backend_write loop already continues. */
    uint32_t chunk = n > 0x40000000u ? 0x40000000u : (uint32_t)n;

    return exo_file_write(handle, buf, chunk);
}

static int efs_close(int handle)
{
    return exo_file_close(handle);
}

static const exo_stdio_backend_t exofs_backend = {
    efs_open, efs_read, efs_seek, efs_close, efs_write, efs_mkdir,
    efs_remove, efs_rename,
};

void exo_file_stdio_install(void)
{
    exo_stdio_set_backend(&exofs_backend);
}

void exo_file_stdio_remove(void)
{
    exo_stdio_set_backend(NULL);
    exo_file_stdio_set_fallback(NULL);
}

void exo_file_stdio_set_fallback(exofs_vol_t *vol)
{
    g_fallback_vol = vol;
    g_fallback_gen = vol != NULL ? vol->gen : 0;
}
