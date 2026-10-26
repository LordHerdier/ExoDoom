#include "libos_fs/exo_file_stdio.h"

#include "exo_errno.h"
#include "libos_fs/exo_file.h"
#include "stdio.h"

#include <stddef.h>
#include <stdint.h>

#define STDIO_PATH_MAX 128

static int efs_open(const char *path)
{
    char abs[STDIO_PATH_MAX];
    size_t n = 0;

    while (path[0] == '.' && path[1] == '/')
        path += 2;

    abs[n++] = '/';
    while (*path == '/')
        path++;
    for (; *path != '\0'; path++) {
        if (n + 1 >= sizeof abs)
            return -EXO_EINVAL;
        abs[n++] = *path;
    }
    abs[n] = '\0';

    return exo_file_open(abs, EXO_FILE_O_READ);
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

static int efs_close(int handle)
{
    return exo_file_close(handle);
}

static const exo_stdio_backend_t exofs_backend = {
    efs_open, efs_read, efs_seek, efs_close,
};

void exo_file_stdio_install(void)
{
    exo_stdio_set_backend(&exofs_backend);
}

void exo_file_stdio_remove(void)
{
    exo_stdio_set_backend(NULL);
}
