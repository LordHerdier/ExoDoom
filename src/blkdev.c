#include "blkdev.h"

#include <stddef.h>

static blkdev_t g_devs[BLKDEV_MAX];
static uint8_t  g_used[BLKDEV_MAX];

int blkdev_register_system(const blkdev_t *dev)
{
    if (g_used[BLKDEV_SYSTEM])
        return BLKDEV_EBUSY;

    g_devs[BLKDEV_SYSTEM] = *dev;
    g_used[BLKDEV_SYSTEM] = 1;
    return (int)BLKDEV_SYSTEM;
}

int blkdev_register(const blkdev_t *dev)
{
    for (uint32_t i = BLKDEV_SYSTEM + 1; i < BLKDEV_MAX; i++) {
        if (!g_used[i]) {
            g_devs[i] = *dev;
            g_used[i] = 1;
            return (int)i;
        }
    }
    return BLKDEV_EFULL;
}

const blkdev_t *blkdev_get(uint32_t index)
{
    return (index < BLKDEV_MAX && g_used[index]) ? &g_devs[index] : NULL;
}

uint32_t blkdev_count(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < BLKDEV_MAX; i++)
        n += g_used[i];
    return n;
}

void blkdev_save(blkdev_snapshot_t *out)
{
    for (uint32_t i = 0; i < BLKDEV_MAX; i++) {
        out->devs[i] = g_devs[i];
        out->used[i] = g_used[i];
    }
}

void blkdev_restore(const blkdev_snapshot_t *in)
{
    for (uint32_t i = 0; i < BLKDEV_MAX; i++) {
        g_devs[i] = in->devs[i];
        g_used[i] = in->used[i];
    }
}

void blkdev_unregister(uint32_t index)
{
    if (index < BLKDEV_MAX)
        g_used[index] = 0;
}
