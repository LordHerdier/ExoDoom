/*
 * test_vmm_fb_wad_k.c — SCRUM-16: the framebuffer aperture reachable
 * through the paged kernel map.
 *
 * Mapped by vmm_init() before run_tests() is ever called, since it sits in
 * the MMIO hole no mmap region covers -- this test is the proof, not the
 * mechanism.
 *
 * This file used to carry a second test, proving the GRUB multiboot WAD
 * module was identity-mapped and its magic bytes readable straight through
 * that mapping. SCRUM-190 retired the module -- the IWAD now arrives via
 * ExoFS/ATA instead (src/doom_wad_disk.h) -- so there is no longer a module
 * for that test to find; coverage for the disk-backed path lives in
 * tests/kernel/test_doom_wad_disk_k.c instead.
 */

#include "kunit.h"
#include "vmm.h"
#include "mmap.h"
#include "multiboot2.h"

#include <stdint.h>

static void test_framebuffer_aperture_is_mapped(void)
{
    const struct mb2_info *mb = mmap_get_info();
    CU_ASSERT_PTR_NOT_NULL(mb);
    if (mb == NULL) return;

    const struct mb2_tag *tag = mb2_find_tag(mb, MB2_TAG_FRAMEBUFFER);
    CU_ASSERT_PTR_NOT_NULL(tag);
    if (tag == NULL) return;

    const struct mb2_tag_framebuffer *fb =
        (const struct mb2_tag_framebuffer *)tag;
    CU_ASSERT_TRUE(fb->addr != 0);
    if (fb->addr == 0) return;

    uint64_t paddr = 0, flags = 0;
    CU_ASSERT_EQUAL(vmm_translate(fb->addr, &paddr, &flags), VMM_OK);
    CU_ASSERT_EQUAL(paddr, fb->addr);
    CU_ASSERT_TRUE((flags & VMM_PRESENT) != 0);

    /* The whole aperture, not just its first byte -- the last byte of the
     * last row is the one a real blit would actually reach. */
    uint64_t last_byte = fb->addr + (uint64_t)fb->pitch * fb->height - 1;
    CU_ASSERT_EQUAL(vmm_translate(last_byte, &paddr, NULL), VMM_OK);
    CU_ASSERT_EQUAL(paddr, last_byte);
}

void suite_vmm_fb_wad_tests(CU_pSuite s)
{
    CU_add_test(s, "framebuffer aperture is mapped end to end",
                test_framebuffer_aperture_is_mapped);
}
