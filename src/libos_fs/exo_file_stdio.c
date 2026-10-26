#include "libos_fs/exo_file_stdio.h"

#include "exo_errno.h"
#include "libos_fs/exo_file.h"
#include "libos_fs/exofs.h"
#include "stdio.h"

#include <stddef.h>
#include <stdint.h>

#define STDIO_PATH_MAX 128

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

    return exo_file_open(abs, f);
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
};

void exo_file_stdio_install(void)
{
    exo_stdio_set_backend(&exofs_backend);
}

void exo_file_stdio_remove(void)
{
    exo_stdio_set_backend(NULL);
}
