/*
 * test_syscall_stat_k.c — exo_memstat / exo_pslist (SCRUM-113).
 *
 * Drives the real dispatch path, exo_syscall_dispatch(EXO_SYS_{MEMSTAT,
 * PSLIST}, ...) — the same route a ring-3 `syscall` takes once the entry
 * stub has marshalled its arguments. Both handlers are bound in kernel_main
 * by syscall_stat_init() before run_tests().
 *
 * The scratch mapping below reuses exo_page_alloc/exo_page_map (already
 * bound), the same reasoning test_syscall_serial_k.c gives: it keeps this
 * suite testing the syscalls' real, user-visible effect (including the
 * SCRUM-186 in-window-but-unmapped EFAULT check) rather than an internal
 * shortcut.
 *
 * The pslist tests create their own scratch contexts via context_create()
 * (same pattern as test_context_k.c) and tear them down again -- CONTEXT_MAX
 * is only 3, shared with every other suite in this one test boot.
 */

#include "kunit.h"
#include "syscall.h"
#include "exo_syscall.h"
#include "context.h"
#include "vmm.h"
#include "page_alloc.h"

#include <stdint.h>

/* Scratch virtual address in the LibOS window, apart from other suites'
 * ranges (test_syscall_serial_k.c uses +0x28000000). */
#define SCRATCH (EXO_USER_VA_BASE + 0x29000000ULL)
#define UNMAPPED (SCRATCH + 0x1000ULL)

static int64_t do_memstat(uint64_t out)
{
    return exo_syscall_dispatch(EXO_SYS_MEMSTAT, out, 0, 0, 0, 0, 0);
}

static int64_t do_pslist(uint64_t out, uint64_t max)
{
    return exo_syscall_dispatch(EXO_SYS_PSLIST, out, max, 0, 0, 0, 0);
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
    return exo_syscall_dispatch(EXO_SYS_PAGE_MAP, vaddr, paddr, flags,
                                0, 0, 0);
}

static int64_t do_unmap(uint64_t vaddr)
{
    return exo_syscall_dispatch(EXO_SYS_PAGE_UNMAP, vaddr, 0, 0, 0, 0, 0);
}

/* Sweep every scratch context this suite's tests might have left live, the
 * same defensive shape test_context_k.c's own cleanup uses -- a failed
 * assertion mid-test must not leak a PML4 into a later suite. */
#define CONTEXT_CLEANUP_SCAN_IDS 4096

int syscall_stat_suite_cleanup(void)
{
    for (uint32_t offset = 0; offset < CONTEXT_CLEANUP_SCAN_IDS; offset++) {
        page_owner_t id = (page_owner_t)(PAGE_OWNER_LIBOS + offset);
        if (context_lookup(id) != NULL) {
            context_destroy(id);
        }
    }
    return 0;
}

static void test_handlers_are_bound(void)
{
    CU_ASSERT_PTR_NOT_NULL(exo_syscall_handler(EXO_SYS_MEMSTAT));
    CU_ASSERT_PTR_NOT_NULL(exo_syscall_handler(EXO_SYS_PSLIST));
}

/* ── memstat ──────────────────────────────────────────────────────────── */

static void test_memstat_null_out_rejected(void)
{
    CU_ASSERT_EQUAL(do_memstat(0), -EXO_EFAULT);
}

static void test_memstat_unmapped_in_window_rejected(void)
{
    /* SCRUM-186 shape: in-window, never exo_page_map'd. */
    CU_ASSERT_EQUAL(do_memstat(UNMAPPED), -EXO_EFAULT);
}

static void test_memstat_reports_consistent_totals(void)
{
    int64_t p = do_alloc();
    CU_ASSERT(p > 0);
    CU_ASSERT_EQUAL(do_map(SCRATCH, (uint64_t)p, EXO_PAGE_WRITE | EXO_PAGE_USER), 0);

    CU_ASSERT_EQUAL(do_memstat(SCRATCH), 0);

    exo_memstat_t *stat = (exo_memstat_t *)(uintptr_t)SCRATCH;
    CU_ASSERT_EQUAL(stat->region_count, page_alloc_region_count());
    CU_ASSERT_EQUAL(stat->total_pages,
                    stat->free_pages + stat->kernel_pages + stat->libos_pages);
    /* The page this test just allocated and mapped is itself LibOS-owned,
     * so the pool can never report itself entirely free while it's live. */
    CU_ASSERT(stat->libos_pages >= 1);

    CU_ASSERT_EQUAL(do_unmap(SCRATCH), 0);
    CU_ASSERT_EQUAL(do_free((uint64_t)p), 0);
}

/* ── pslist ───────────────────────────────────────────────────────────── */

static void test_pslist_max_over_cap_rejected(void)
{
    CU_ASSERT_EQUAL(do_pslist(SCRATCH, EXO_PSLIST_MAX + 1), -EXO_EINVAL);
}

static void test_pslist_null_out_rejected(void)
{
    CU_ASSERT_EQUAL(do_pslist(0, 1), -EXO_EFAULT);
}

static void test_pslist_zero_max_is_a_noop(void)
{
    /* max == 0 needs no valid out at all -- there is nothing to write. */
    CU_ASSERT_EQUAL(do_pslist(0, 0), 0);
}

static void test_pslist_unmapped_in_window_rejected(void)
{
    CU_ASSERT_EQUAL(do_pslist(UNMAPPED, 1), -EXO_EFAULT);
}

static uint64_t new_address_space(void)
{
    uint64_t phys = 0;
    CU_ASSERT_EQUAL(vmm_create_address_space(&phys), VMM_OK);
    CU_ASSERT_NOT_EQUAL(phys, 0);
    return phys;
}

static void test_pslist_reports_created_context(void)
{
    page_owner_t id;
    CU_ASSERT_EQUAL(context_create(new_address_space(), &id), CONTEXT_OK);

    int64_t p = do_alloc();
    CU_ASSERT(p > 0);
    CU_ASSERT_EQUAL(do_map(SCRATCH, (uint64_t)p, EXO_PAGE_WRITE | EXO_PAGE_USER), 0);

    int64_t rc = do_pslist(SCRATCH, EXO_PSLIST_MAX);
    CU_ASSERT(rc >= 1);

    exo_ps_info_t *procs = (exo_ps_info_t *)(uintptr_t)SCRATCH;
    int found = 0;
    for (int64_t i = 0; i < rc; i++) {
        if (procs[i].id == id) {
            found = 1;
            /* Freshly created, never switched to -- CONTEXT_STATE_READY. */
            CU_ASSERT_EQUAL(procs[i].state, 1);
        }
    }
    CU_ASSERT(found);

    CU_ASSERT_EQUAL(do_unmap(SCRATCH), 0);
    CU_ASSERT_EQUAL(do_free((uint64_t)p), 0);
    CU_ASSERT_EQUAL(context_destroy(id), CONTEXT_OK);
}

static void test_pslist_page_count_matches_owner(void)
{
    page_owner_t id;
    CU_ASSERT_EQUAL(context_create(new_address_space(), &id), CONTEXT_OK);

    /* Stamp one page as owned by the new context directly, mirroring what a
     * real exo_page_alloc from that context would do -- no ring-3 harness
     * needed just to attribute a page to a non-current context. */
    void *owned = alloc_page_owned(id);
    CU_ASSERT_PTR_NOT_NULL(owned);

    int64_t p = do_alloc();
    CU_ASSERT(p > 0);
    CU_ASSERT_EQUAL(do_map(SCRATCH, (uint64_t)p, EXO_PAGE_WRITE | EXO_PAGE_USER), 0);

    int64_t rc = do_pslist(SCRATCH, EXO_PSLIST_MAX);
    CU_ASSERT(rc >= 1);

    exo_ps_info_t *procs = (exo_ps_info_t *)(uintptr_t)SCRATCH;
    int found = 0;
    for (int64_t i = 0; i < rc; i++) {
        if (procs[i].id == id) {
            found = 1;
            CU_ASSERT_EQUAL(procs[i].page_count, 1u);
        }
    }
    CU_ASSERT(found);

    CU_ASSERT_EQUAL(do_unmap(SCRATCH), 0);
    CU_ASSERT_EQUAL(do_free((uint64_t)p), 0);
    CU_ASSERT_EQUAL(free_page_owned(owned, id), PAGE_FREE_OK);
    CU_ASSERT_EQUAL(context_destroy(id), CONTEXT_OK);
}

void suite_syscall_stat_tests(CU_pSuite s)
{
    CU_add_test(s, "handlers are bound", test_handlers_are_bound);

    CU_add_test(s, "memstat: NULL out rejected", test_memstat_null_out_rejected);
    CU_add_test(s, "memstat: unmapped in-window rejected",
               test_memstat_unmapped_in_window_rejected);
    CU_add_test(s, "memstat: reports consistent totals",
               test_memstat_reports_consistent_totals);

    CU_add_test(s, "pslist: max over cap rejected", test_pslist_max_over_cap_rejected);
    CU_add_test(s, "pslist: NULL out rejected", test_pslist_null_out_rejected);
    CU_add_test(s, "pslist: zero max is a no-op", test_pslist_zero_max_is_a_noop);
    CU_add_test(s, "pslist: unmapped in-window rejected",
               test_pslist_unmapped_in_window_rejected);
    CU_add_test(s, "pslist: reports created context",
               test_pslist_reports_created_context);
    CU_add_test(s, "pslist: page count matches owner",
               test_pslist_page_count_matches_owner);
}
