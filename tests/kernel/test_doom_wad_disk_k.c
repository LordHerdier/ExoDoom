/*
 * test_doom_wad_disk_k.c — SCRUM-190: the IWAD loads off the real ExoFS
 * volume on build/exodoom_disk.img, through doom_wad_disk_get().
 *
 * This is the acceptance test for the mechanism src/syscall_launch.c now
 * depends on for every LAUNCH_NEEDS_WAD app (replacing the old
 * mmap_find_module()/GRUB-module lookup): mount the volume at
 * LIBOS_WAD_DISK_BASE_LBA, open LIBOS_WAD_FILE_NAME, read it into a
 * contiguous physical buffer, and cache the result. Driven from ring 0
 * exactly the way test_exofs_k.c drives the rest of ExoFS -- no ring-3
 * target needed, since doom_wad_disk_get() is kernel-only code
 * (src/doom_wad_disk.c's own comment explains why).
 *
 * Soft-fails like every other disk test here (test_ata_k.c,
 * test_exofs_k.c): a dev machine with no QEMU drive attached gets
 * -EXO_ENODEV, which these tests treat as "nothing to check" rather than a
 * failure, the same way disk_binding_present()-gated tests elsewhere do.
 */

#include "kunit.h"
#include "doom_wad_disk.h"
#include "ata.h"
#include "exo_errno.h"
#include "page_alloc.h"
#include "vmm.h"
#include "string.h"

#include <stdint.h>

static int drive_present(void)
{
    return ata_init() == ATA_OK;
}

/*
 * The real freedoom2.wad, fetched and verified at build time
 * (docker/scripts/build.sh step [1/7]) and written onto the volume by step
 * [1b/7] -- so this is always an IWAD, never a synthetic header. The
 * malformed-input rejection paths belong to tests/kernel/test_doom_wad_k.c,
 * which builds those by hand; this suite only has the one real file.
 */
static void test_loads_real_iwad(void)
{
    if (!drive_present()) return;

    uint64_t paddr = 0, size = 0;
    int64_t  rc = doom_wad_disk_get(&paddr, &size);

    CU_ASSERT_EQUAL(rc, 0);
    if (rc != 0) return;

    CU_ASSERT_TRUE(paddr != 0);
    CU_ASSERT_TRUE(size > 12); /* at least a WAD header's worth */

    /* Identity-mapped kernel memory (alloc_pages_contig_owned() hands back
     * PAGE_OWNER_KERNEL pages, same tag the old multiboot module's pages
     * carried), so the physical address doubles as a readable pointer. */
    const char *magic = (const char *)(uintptr_t)paddr;
    CU_ASSERT_TRUE(memcmp(magic, "IWAD", 4) == 0 ||
                   memcmp(magic, "PWAD", 4) == 0);

    /* Every page of the buffer is tagged PAGE_OWNER_KERNEL -- the same
     * permanence contract the old GRUB module's pages had (page_alloc.h's
     * own comment), which is what makes it safe for libos_map_wad() to map
     * this buffer read-only into more than one LibOS over the kernel's
     * lifetime. */
    CU_ASSERT_EQUAL(page_owner((void *)(uintptr_t)paddr), PAGE_OWNER_KERNEL);
}

static void test_second_call_returns_cache(void)
{
    if (!drive_present()) return;

    uint64_t paddr1 = 0, size1 = 0, paddr2 = 0, size2 = 0;

    CU_ASSERT_EQUAL(doom_wad_disk_get(&paddr1, &size1), 0);
    CU_ASSERT_EQUAL(doom_wad_disk_get(&paddr2, &size2), 0);

    /* Exactly the same buffer, not a second ~28 MB read+alloc -- see
     * doom_wad_disk.h's own comment for why a second allocation per launch
     * would exhaust a 256 MiB VM's PMM after a handful of relaunches. */
    CU_ASSERT_EQUAL(paddr1, paddr2);
    CU_ASSERT_EQUAL(size1, size2);
}

void suite_doom_wad_disk_tests(CU_pSuite s)
{
    CU_add_test(s, "the real IWAD loads off the ExoFS-backed disk image",
                test_loads_real_iwad);
    CU_add_test(s, "a second call returns the cached buffer, not a fresh read",
                test_second_call_returns_cache);
}
