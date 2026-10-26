#include "exofs_attach.h"
#include "exofs_blockdev.h"
#include "exofs_layout.h"

#include "exo_errno.h"
#include "libos_heap.h"

#include <stddef.h>
#include <stdint.h>

int exofs_sector_is_blank(const void *sector, size_t len)
{
    const uint8_t *p = sector;

    if (p == NULL)
        return 0;
    for (size_t i = 0; i < len; i++)
        if (p[i] != 0)
            return 0;
    return 1;
}

int exofs_attach(uint32_t dev, uint32_t base_lba, uint32_t sectors,
                 exofs_vol_t **out)
{
    int mrc = exofs_mount(dev, base_lba, out);
    if (mrc == 0)
        return EXOFS_ATTACH_MOUNTED;

    /* Only "not a volume" can mean "not formatted yet". Already mounted, no
     * free slot, no memory, a device fault: none of those is a reason to
     * write to the disk. */
    if (mrc != -EXO_EINVAL)
        return mrc;

    uint8_t *sector = libos_heap_alloc(EXOFS_BLOCK_SIZE);
    if (sector == NULL)
        return -EXO_ENOMEM;

    int rc    = exofs_bdev_read(dev, base_lba, sector, 1);
    int blank = rc >= 0 && exofs_sector_is_blank(sector, EXOFS_BLOCK_SIZE);
    libos_heap_free(sector);

    if (rc < 0)
        return rc;
    if (!blank)
        return mrc;   /* somebody's data: report, never format over it */

    rc = exofs_format(dev, base_lba, sectors);
    if (rc < 0)
        return rc;
    rc = exofs_mount(dev, base_lba, out);
    if (rc < 0)
        return rc;
    return EXOFS_ATTACH_FORMATTED;
}
