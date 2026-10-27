/*
 * doom_wad_disk.c — see doom_wad_disk.h.
 *
 * Kernel-only (EXO_KERNEL): alloc_pages_contig_owned() is a PMM primitive
 * with no ring-3 equivalent, and exofs_bdev_acquire()/etc. only avoid a
 * ring-3 CPL drop (exofs_blockdev.c's own comment) because this file is
 * always compiled with -DEXO_KERNEL, never linked into a ring-3 target.
 */

#include "doom_wad_disk.h"

#include "libos_fs/exofs.h"
#include "libos_fs/exofs_blockdev.h"
#include "libos_wad_params.h"
#include "page_alloc.h"
#include "vmm.h"
#include "exo_errno.h"
#include "exo_volume.h"

#include <stdint.h>

static uint64_t g_wad_paddr = 0;
static uint64_t g_wad_size  = 0;

int64_t doom_wad_disk_get(uint64_t *out_paddr, uint64_t *out_size)
{
    if (g_wad_size != 0) {
        *out_paddr = g_wad_paddr;
        *out_size  = g_wad_size;
        return 0;
    }

    /* Only the program volume, not the whole disk: a program holding its own
     * volume extent (SCRUM-237) must not make the IWAD unreadable. */
    int rc = exofs_bdev_acquire_extent(0, LIBOS_WAD_DISK_BASE_LBA,
                                       EXO_VOLUME_REGION_BASE_LBA -
                                           LIBOS_WAD_DISK_BASE_LBA);
    if (rc != 0) {
        return rc;
    }

    exofs_vol_t *vol = NULL;
    rc = exofs_mount(0, LIBOS_WAD_DISK_BASE_LBA, &vol);
    if (rc != 0) {
        exofs_bdev_release(0);
        return rc;
    }

    exofs_file_t f;
    rc = exofs_open(vol, "/" LIBOS_WAD_FILE_NAME, EXOFS_O_READ, &f);
    if (rc != 0) {
        exofs_unmount(vol);
        exofs_bdev_release(0);
        return rc;
    }

    uint32_t size  = f.size;
    uint32_t pages = (size + (uint32_t)VMM_PAGE_SIZE - 1) / (uint32_t)VMM_PAGE_SIZE;
    if (pages == 0) {
        pages = 1; /* alloc_pages_contig_owned(_, 0) is a defined failure */
    }

    void *buf = alloc_pages_contig_owned(PAGE_OWNER_KERNEL, pages);
    if (buf == NULL) {
        exofs_close(&f);
        exofs_unmount(vol);
        exofs_bdev_release(0);
        return -EXO_ENOMEM;
    }

    int64_t got = exofs_read(&f, buf, size);
    exofs_close(&f);
    exofs_unmount(vol);
    exofs_bdev_release(0);

    if (got < 0 || (uint64_t)got != (uint64_t)size) {
        /* Not cached, so this is the only reference -- free it. No bulk
         * free exists: each page is tagged individually, same as
         * alloc_page_owned(), so it is freed the same way, one at a time. */
        for (uint32_t i = 0; i < pages; i++) {
            free_page_owned((uint8_t *)buf + (uint64_t)i * VMM_PAGE_SIZE,
                            PAGE_OWNER_KERNEL);
        }
        return (got < 0) ? got : -EXO_EIO;
    }

    g_wad_paddr = (uint64_t)(uintptr_t)buf;
    g_wad_size  = size;
    *out_paddr  = g_wad_paddr;
    *out_size   = g_wad_size;
    return 0;
}
