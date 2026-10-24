#include "blkdev.h"

#include <stddef.h>

static blkdev_t g_devs[BLKDEV_MAX];
static uint32_t g_count;

int blkdev_register(const blkdev_t *dev)
{
    if (g_count >= BLKDEV_MAX)
        return BLKDEV_EFULL;

    g_devs[g_count] = *dev;
    return (int)g_count++;
}

const blkdev_t *blkdev_get(uint32_t index)
{
    return index < g_count ? &g_devs[index] : NULL;
}

uint32_t blkdev_count(void)
{
    return g_count;
}

void blkdev_truncate(uint32_t n)
{
    if (n < g_count)
        g_count = n;
}
