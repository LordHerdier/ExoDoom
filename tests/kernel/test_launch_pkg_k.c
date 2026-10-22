/*
 * test_launch_pkg_k.c — SCRUM-230: EXO_SYS_LAUNCH_PKG (#36), launching a
 * LibOS from the bytes of an exo_pkg.h package in the caller's memory
 * instead of from a blob linked into the kernel.
 *
 * Drives the real, bound sys_launch_pkg() handler through
 * exo_syscall_dispatch(), the same direct-dispatch pattern
 * test_launch_multi_k.c uses for #21. The package lives in a page mapped into
 * the LibOS window through the real #1/#2 syscalls: the handler's
 * exo_range_in_user_window()/exo_user_range_mapped() pair refuses a kernel
 * pointer, so a stack buffer would test nothing but the rejection.
 *
 * What the suite is for:
 *   - every refusal happens BEFORE a context row or page is taken (the free
 *     page count and the context count are unchanged), so a hostile or
 *     corrupt package costs nothing;
 *   - a good package really builds an image from the caller's bytes and the
 *     result no longer depends on the caller's buffer;
 *   - the framebuffer-release decision follows the manifest.
 */

#include "kunit.h"
#include "context.h"
#include "vmm.h"
#include "page_alloc.h"
#include "libos_launch.h"
#include "exo_pkg.h"
#include "exo_syscall.h"
#include "syscall.h"
#include "string.h"

#include <stdint.h>

extern void libos_launch_probe(void);
extern void libos_launch_probe_end(void);

/* Apart from every other suite's scratch range (test_syscall_sound_stream_k.c
 * uses +0x2E000000). */
#define PKG_SCRATCH  (EXO_USER_VA_BASE + 0x2F000000ULL)
/* In-window, never mapped by this suite. */
#define PKG_UNMAPPED (PKG_SCRATCH + 0x10000ULL)

/* Same shape as launch_multi_suite_cleanup(): a failed assertion mid-test
 * must not leak a context (and its pages) into a later suite, and a real #36
 * success arms context_switch_pending, which nothing here commits. */
#define LAUNCH_PKG_CLEANUP_SCAN_IDS 4096

int launch_pkg_suite_cleanup(void)
{
    for (uint32_t offset = 0; offset < LAUNCH_PKG_CLEANUP_SCAN_IDS; offset++) {
        page_owner_t id = (page_owner_t)(PAGE_OWNER_LIBOS + offset);
        if (context_lookup(id) != NULL) {
            page_reclaim_all(id);
            context_destroy(id);
        }
    }
    context_switch_pending = 0;
    context_set_current(PAGE_OWNER_LIBOS);
    return 0;
}

static int64_t do_launch_pkg(uint64_t buf, uint64_t len)
{
    return exo_syscall_dispatch(EXO_SYS_LAUNCH_PKG, buf, len, 0, 0, 0, 0);
}

static int64_t do_alloc(void)
{
    return exo_syscall_dispatch(EXO_SYS_PAGE_ALLOC, 0, 0, 0, 0, 0, 0);
}

static int64_t do_free(uint64_t paddr)
{
    return exo_syscall_dispatch(EXO_SYS_PAGE_FREE, paddr, 0, 0, 0, 0, 0);
}

static int64_t do_map(uint64_t vaddr, uint64_t paddr, uint64_t flags)
{
    return exo_syscall_dispatch(EXO_SYS_PAGE_MAP, vaddr, paddr, flags, 0, 0, 0);
}

static int64_t do_unmap(uint64_t vaddr)
{
    return exo_syscall_dispatch(EXO_SYS_PAGE_UNMAP, vaddr, 0, 0, 0, 0, 0);
}

static uint32_t free_pages_now(void)
{
    uint32_t total, freep, kern, libos;
    page_alloc_totals(&total, &freep, &kern, &libos);
    return freep;
}

/* Map one page at PKG_SCRATCH, zeroed. Returns its physical address for
 * unmap_scratch(). */
static uint64_t map_scratch(void)
{
    int64_t paddr = do_alloc();
    CU_ASSERT_TRUE(paddr > 0);
    if (paddr <= 0)
        return 0;
    CU_ASSERT_EQUAL(do_map(PKG_SCRATCH, (uint64_t)paddr,
                           EXO_PAGE_READ | EXO_PAGE_WRITE | EXO_PAGE_USER), 0);
    memset((void *)(uintptr_t)PKG_SCRATCH, 0, VMM_PAGE_SIZE);
    return (uint64_t)paddr;
}

static void unmap_scratch(uint64_t paddr)
{
    CU_ASSERT_EQUAL(do_unmap(PKG_SCRATCH), 0);
    CU_ASSERT_EQUAL(do_free(paddr), 0);
}

/* A valid header for a code-only program (the launch probe), as the packer
 * would write it. Callers corrupt one field at a time. */
static exo_pkg_header_t good_header(size_t code_len)
{
    exo_pkg_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic             = EXO_PKG_MAGIC;
    h.version           = EXO_PKG_VERSION;
    h.code_len          = (uint32_t)code_len;
    h.stack_pages       = EXO_PKG_MAX_STACK_PAGES;
    h.min_pages         = 20;
    h.recommended_pages = 36;
    return h;
}

static size_t probe_len(void)
{
    return (uintptr_t)&libos_launch_probe_end - (uintptr_t)&libos_launch_probe;
}

/* Write header + the launch probe's code at PKG_SCRATCH; returns the total
 * length. */
static size_t write_probe_package(const exo_pkg_header_t *h)
{
    size_t total = 0;
    CU_ASSERT_EQUAL(exo_pkg_pack((void *)(uintptr_t)PKG_SCRATCH, VMM_PAGE_SIZE,
                                 h, (const void *)&libos_launch_probe, NULL,
                                 &total), 0);
    return total;
}

/* The "shell" stand-in: a real, live, current context, built the way
 * test_launch_multi_k.c's make_foreground() does, because
 * context_switch_request() refuses to arm a switch without one. */
static page_owner_t make_foreground(void)
{
    page_owner_t id = PAGE_OWNER_FREE;
    CU_ASSERT_EQUAL(context_create(vmm_kernel_pml4(), &id), CONTEXT_OK);

    libos_image_t img;
    CU_ASSERT_EQUAL(libos_build_image(id, (const void *)&libos_launch_probe,
                                      probe_len(), 0, 0, 0, &img), VMM_OK);
    CU_ASSERT_EQUAL(context_prime(id, img.entry_vaddr, img.stack_top_vaddr),
                    CONTEXT_OK);
    context_set_current(id);
    context_set_state(id, CONTEXT_STATE_RUNNING);
    return id;
}

/* Put the world back the way make_foreground() found it. The scratch page
 * belongs to PAGE_OWNER_LIBOS (the context the suite starts as), and #3/#1
 * answer -EXO_EPERM to anyone else, so the unmap that follows has to run as
 * that owner again. */
static void drop_foreground(page_owner_t fg)
{
    context_switch_pending = 0;
    context_set_current(PAGE_OWNER_LIBOS);
    page_reclaim_all(fg);
    context_destroy(fg);
}

/* ---- Shape: the length is judged before the pointer is ever walked ----- */

static void test_rejects_lengths_that_cannot_be_a_package(void)
{
    /* PKG_UNMAPPED is not mapped: if any of these reached the buffer check
     * they would answer -EXO_EFAULT instead. */
    CU_ASSERT_EQUAL(do_launch_pkg(PKG_UNMAPPED, 0), -EXO_EINVAL);
    CU_ASSERT_EQUAL(do_launch_pkg(PKG_UNMAPPED, sizeof(exo_pkg_header_t) - 1),
                    -EXO_EINVAL);
    CU_ASSERT_EQUAL(do_launch_pkg(PKG_UNMAPPED, EXO_LAUNCH_PKG_MAX_LEN + 1),
                    -EXO_EINVAL);
    CU_ASSERT_EQUAL(do_launch_pkg(PKG_UNMAPPED, ~0ULL), -EXO_EINVAL);
}

/* ---- Buffer: in the window AND mapped ---------------------------------- */

static void test_rejects_unmapped_and_out_of_window_buffers(void)
{
    CU_ASSERT_EQUAL(do_launch_pkg(PKG_UNMAPPED, sizeof(exo_pkg_header_t)),
                    -EXO_EFAULT);
    CU_ASSERT_EQUAL(do_launch_pkg(EXO_USER_VA_BASE - 1,
                                  sizeof(exo_pkg_header_t)), -EXO_EFAULT);
    CU_ASSERT_EQUAL(do_launch_pkg(0, sizeof(exo_pkg_header_t)), -EXO_EFAULT);
}

static void test_rejects_buffer_crossing_into_unmapped(void)
{
    uint64_t paddr = map_scratch();
    exo_pkg_header_t h = good_header(probe_len());
    write_probe_package(&h);

    /* The package is valid and fits in the one mapped page; claiming two
     * pages of it must be refused on the second page, not read through. */
    CU_ASSERT_EQUAL(do_launch_pkg(PKG_SCRATCH, 2 * VMM_PAGE_SIZE),
                    -EXO_EFAULT);
    unmap_scratch(paddr);
}

/* ---- Contents: a malformed package is refused and costs nothing -------- */

/* Pack a good package, let `corrupt` break the copy sitting in the buffer,
 * and launch it. */
typedef void (*corrupt_fn)(exo_pkg_header_t *h);

static int64_t launch_corrupted(corrupt_fn corrupt, size_t *len_out)
{
    exo_pkg_header_t h = good_header(probe_len());
    size_t total = write_probe_package(&h);
    exo_pkg_header_t *in_buf = (exo_pkg_header_t *)(uintptr_t)PKG_SCRATCH;
    corrupt(in_buf);
    if (len_out)
        *len_out = total;
    return do_launch_pkg(PKG_SCRATCH, *len_out);
}

static void corrupt_magic(exo_pkg_header_t *h)    { h->magic ^= 1u; }
static void corrupt_version(exo_pkg_header_t *h)  { h->version += 1u; }
static void corrupt_reserved(exo_pkg_header_t *h) { h->reserved[2] = 1u; }
static void corrupt_no_code(exo_pkg_header_t *h)  { h->code_len = 0; }
static void corrupt_oversize(exo_pkg_header_t *h)
{
    h->code_len = EXO_PKG_MAX_CODE_PAGES * EXO_PKG_PAGE_SIZE + 1u;
}
/* Claims more code than the buffer holds. */
static void corrupt_lying_length(exo_pkg_header_t *h)
{
    h->code_len = (uint32_t)VMM_PAGE_SIZE;
}
static void corrupt_want_flags(exo_pkg_header_t *h) { h->want_flags = 1u << 9; }

static void test_malformed_packages_are_refused_without_side_effects(void)
{
    static const corrupt_fn cases[] = {
        corrupt_magic,      corrupt_version, corrupt_reserved,
        corrupt_no_code,    corrupt_oversize, corrupt_lying_length,
        corrupt_want_flags,
    };
    uint64_t paddr = map_scratch();
    page_owner_t fg = make_foreground();

    uint32_t free_before = free_pages_now();
    uint32_t ctx_before  = context_count();

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        size_t len = 0;
        CU_ASSERT_EQUAL(launch_corrupted(cases[i], &len), -EXO_EINVAL);
    }

    CU_ASSERT_EQUAL(free_pages_now(), free_before);
    CU_ASSERT_EQUAL(context_count(), ctx_before);
    CU_ASSERT_EQUAL(context_switch_pending, 0);

    drop_foreground(fg);
    unmap_scratch(paddr);
}

/* ---- A good package launches, and owns its own copy -------------------- */

static void test_good_package_builds_an_image_from_the_callers_bytes(void)
{
    uint64_t paddr = map_scratch();
    page_owner_t fg = make_foreground();
    exo_pkg_header_t h = good_header(probe_len());
    size_t total = write_probe_package(&h);

    context_info_t before[CONTEXT_MAX];
    uint32_t n_before = context_list(before, CONTEXT_MAX);

    CU_ASSERT_EQUAL(do_launch_pkg(PKG_SCRATCH, total), 0);
    CU_ASSERT_EQUAL(context_switch_pending, 1);

    context_info_t after[CONTEXT_MAX];
    uint32_t n_after = context_list(after, CONTEXT_MAX);
    CU_ASSERT_EQUAL(n_after, n_before + 1);

    page_owner_t new_id = PAGE_OWNER_FREE;
    for (uint32_t i = 0; i < n_after; i++) {
        int known = 0;
        for (uint32_t j = 0; j < n_before; j++)
            known |= (after[i].id == before[j].id);
        if (!known)
            new_id = after[i].id;
    }
    CU_ASSERT_NOT_EQUAL(new_id, PAGE_OWNER_FREE);

    /* The context owns its pages: one code page and the stack, at least. */
    CU_ASSERT_TRUE(page_count_owned(new_id) >= 1u + EXO_PKG_MAX_STACK_PAGES);

    /* Trash the caller's buffer. The launched image must already hold its own
     * copy of the code, byte for byte -- the buffer is the caller's again the
     * moment the syscall returns. */
    memset((void *)(uintptr_t)PKG_SCRATCH, 0xFF, VMM_PAGE_SIZE);

    uint64_t code_paddr = 0;
    CU_ASSERT_EQUAL(vmm_translate_in((uint64_t *)(uintptr_t)context_pml4(new_id),
                                     LIBOS_LAUNCH_CODE_VADDR, &code_paddr,
                                     NULL), 0);
    CU_ASSERT_EQUAL(memcmp((const void *)(uintptr_t)code_paddr,
                           (const void *)&libos_launch_probe, probe_len()), 0);

    context_switch_pending = 0;
    drop_foreground(fg);
    unmap_scratch(paddr);
}

/* A good package must also be reclaimable like any other launch: destroying
 * it returns every page it took. */
static void test_launched_package_is_fully_reclaimable(void)
{
    uint64_t paddr = map_scratch();
    page_owner_t fg = make_foreground();
    exo_pkg_header_t h = good_header(probe_len());
    size_t total = write_probe_package(&h);

    uint32_t free_before = free_pages_now();
    context_info_t before[CONTEXT_MAX];
    uint32_t n_before = context_list(before, CONTEXT_MAX);

    CU_ASSERT_EQUAL(do_launch_pkg(PKG_SCRATCH, total), 0);

    context_info_t after[CONTEXT_MAX];
    uint32_t n_after = context_list(after, CONTEXT_MAX);
    for (uint32_t i = 0; i < n_after; i++) {
        int known = 0;
        for (uint32_t j = 0; j < n_before; j++)
            known |= (after[i].id == before[j].id);
        if (!known) {
            page_reclaim_all(after[i].id);
            context_destroy(after[i].id);
        }
    }
    context_switch_pending = 0;
    context_set_current(fg);

    CU_ASSERT_EQUAL(free_pages_now(), free_before);
    drop_foreground(fg);
    unmap_scratch(paddr);
}

void suite_launch_pkg_tests(CU_pSuite s)
{
    CU_add_test(s, "lengths that cannot be a package are refused first",
                test_rejects_lengths_that_cannot_be_a_package);
    CU_add_test(s, "unmapped and out-of-window buffers are refused",
                test_rejects_unmapped_and_out_of_window_buffers);
    CU_add_test(s, "a buffer running into unmapped memory is refused",
                test_rejects_buffer_crossing_into_unmapped);
    CU_add_test(s, "malformed packages are refused without side effects",
                test_malformed_packages_are_refused_without_side_effects);
    CU_add_test(s, "a good package builds an image from the caller's bytes",
                test_good_package_builds_an_image_from_the_callers_bytes);
    CU_add_test(s, "a launched package is fully reclaimable",
                test_launched_package_is_fully_reclaimable);
}
