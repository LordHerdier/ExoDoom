/*
 * host_blockdev.c — exofs_blockdev's host-side implementation (SCRUM-190).
 * See host_blockdev.h for why this exists.
 *
 * Deliberately compiled WITHOUT "-I src": every error return here is a
 * plain -1 rather than an EXO_E* code from src/exo_errno.h, because the
 * ExoFS sources only ever test these for "< 0" / "!= 0" (see e.g.
 * exofs_volume.c's mount/format), never for a specific value, so there is
 * nothing to gain from pulling that header in here -- and pulling in
 * "-I src" at all would shadow this file's own <stdio.h> with the
 * project's freestanding src/stdio.h, which declares no FILE type or
 * fopen().
 */

#include "host_blockdev.h"
#include "exofs_blockdev.h"
#include "exofs_layout.h"

#include <stdio.h>

static FILE *g_img = NULL;

int host_blockdev_open(const char *path, uint32_t total_sectors)
{
    g_img = fopen(path, "w+b");
    if (!g_img) {
        return -1;
    }

    static const uint8_t zero[EXOFS_BLOCK_SIZE] = {0};
    for (uint32_t i = 0; i < total_sectors; i++) {
        if (fwrite(zero, EXOFS_BLOCK_SIZE, 1, g_img) != 1) {
            fclose(g_img);
            g_img = NULL;
            return -1;
        }
    }
    if (fflush(g_img) != 0) {
        fclose(g_img);
        g_img = NULL;
        return -1;
    }
    return 0;
}

void host_blockdev_close(void)
{
    if (g_img) {
        fclose(g_img);
        g_img = NULL;
    }
}

int exofs_bdev_acquire(void)
{
    return 0;
}

int exofs_bdev_acquire_extent(uint32_t base, uint32_t count)
{
    (void)base;
    (void)count;
    return 0;
}

void exofs_bdev_release(void)
{
}

uint32_t exofs_bdev_ticks(void)
{
    return 0;
}

int exofs_bdev_read(uint32_t lba, void *buf, uint32_t count)
{
    if (count == 0) return 0;
    if (!g_img || buf == NULL) return -1;

    if (fseek(g_img, (long)lba * EXOFS_BLOCK_SIZE, SEEK_SET) != 0) {
        return -1;
    }
    size_t want = (size_t)count * EXOFS_BLOCK_SIZE;
    if (fread(buf, 1, want, g_img) != want) {
        return -1;
    }
    return 0;
}

int exofs_bdev_write(uint32_t lba, const void *buf, uint32_t count)
{
    if (count == 0) return 0;
    if (!g_img || buf == NULL) return -1;

    if (fseek(g_img, (long)lba * EXOFS_BLOCK_SIZE, SEEK_SET) != 0) {
        return -1;
    }
    size_t want = (size_t)count * EXOFS_BLOCK_SIZE;
    if (fwrite(buf, 1, want, g_img) != want) {
        return -1;
    }
    if (fflush(g_img) != 0) {
        return -1;
    }
    return 0;
}
