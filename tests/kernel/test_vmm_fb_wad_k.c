/*
 * test_vmm_fb_wad_k.c — SCRUM-16: framebuffer + WAD module reachable through
 * the paged kernel map.
 *
 * Both are already mapped by vmm_init() before run_tests() is ever called
 * (the WAD incidentally, as part of every usable RAM region; the framebuffer
 * aperture explicitly, since it sits in the MMIO hole no mmap region covers)
 * — these tests are the proof, not the mechanism. The WAD one goes one step
 * further than a translate check and actually reads the magic bytes back
 * through the mapped virtual address, which is what "WAD parsing still
 * works" after paging actually depends on.
 */

#include "kunit.h"
#include "vmm.h"
#include "mmap.h"
#include "multiboot2.h"
#include "string.h"

#include <stdint.h>

static void test_wad_module_is_mapped_and_readable(void)
{
    uint64_t start = 0, end = 0;
    CU_ASSERT_EQUAL(mmap_find_module(&start, &end), 0);
    CU_ASSERT_TRUE(end > start);
    if (end <= start) return;

    uint64_t paddr = 0, flags = 0;
    CU_ASSERT_EQUAL(vmm_translate(start, &paddr, &flags), VMM_OK);
    CU_ASSERT_EQUAL(paddr, start);
    CU_ASSERT_TRUE((flags & VMM_PRESENT) != 0);

    /* Identity-mapped, so the physical and virtual addresses are the same
     * number -- read the WAD's magic straight through it. */
    const char *magic = (const char *)(uintptr_t)start;
    CU_ASSERT_TRUE(memcmp(magic, "IWAD", 4) == 0 ||
                   memcmp(magic, "PWAD", 4) == 0);
}

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
    CU_add_test(s, "WAD module is mapped and readable through its VA",
                test_wad_module_is_mapped_and_readable);
    CU_add_test(s, "framebuffer aperture is mapped end to end",
                test_framebuffer_aperture_is_mapped);
}
