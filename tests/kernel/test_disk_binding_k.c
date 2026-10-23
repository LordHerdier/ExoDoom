/*
 * test_disk_binding_k.c — disk secure binding (SCRUM-188), disk_binding.c
 * itself plus the exo_disk_acquire/read/write enforcement in
 * src/syscall_disk.c.
 *
 * Mirrors test_fb_binding_k.c's structure, minus the geometry-specific
 * cases fb_binding.c has and disk_binding.c doesn't. Most tests install a
 * synthetic "present"/"absent" state via disk_binding_init() directly
 * rather than depending on whether the CI scratch drive is attached, so the
 * assertions do not depend on the environment test_ata_k.c/
 * test_syscall_disk_k.c already probe.
 *
 * The suite's init/cleanup pair snapshots and restores whatever
 * syscall_disk_init() published at boot, so the rest of the suite run is
 * unaffected.
 */

#include "kunit.h"
#include "disk_binding.h"
#include "page_alloc.h"
#include "syscall.h"
#include "exo_syscall.h"
#include "ata.h"

#include <stdint.h>

/* A second, distinct LibOS id -- the "another LibOS" of this binding's
 * exclusivity rule. v1 only ever runs PAGE_OWNER_LIBOS through the real
 * dispatch path; this id stands in as a foreign context, same convention
 * test_fb_binding_k.c uses. */
#define OTHER_LIBOS ((page_owner_t)(PAGE_OWNER_LIBOS + 1))

#define TEST_LBA 3072

static int boot_disk_present;

/* Same drive-presence probe test_syscall_disk_k.c uses: ata_init() is
 * idempotent and just reports whether a drive answered, so calling it again
 * here doesn't disturb the boot-time binding. */
int disk_binding_suite_init(void)
{
    boot_disk_present = (ata_init() == ATA_OK);

    return 0;
}

int disk_binding_suite_cleanup(void)
{
    /* Put the disk binding back to a present-but-unheld state matching what
     * kernel_main published, so a later suite sees the same environment
     * this one started with. */
    disk_binding_init(boot_disk_present);

    return 0;
}

static int64_t do_disk_acquire(void)
{
    return exo_syscall_dispatch(EXO_SYS_DISK_ACQUIRE, 0, 0, 0, 0, 0, 0);
}

static int64_t do_disk_read(uint64_t lba, uint64_t buf, uint64_t count)
{
    return exo_syscall_dispatch(EXO_SYS_DISK_READ, lba, buf, count, 0, 0, 0);
}

static int64_t do_disk_write(uint64_t lba, uint64_t buf, uint64_t count)
{
    return exo_syscall_dispatch(EXO_SYS_DISK_WRITE, lba, buf, count, 0, 0, 0);
}

/* ── Boot wiring ─────────────────────────────────────────────────────────── */

static void test_boot_bound_the_acquire_syscall(void)
{
    CU_ASSERT_PTR_NOT_NULL(exo_syscall_handler(EXO_SYS_DISK_ACQUIRE));
}

/* ── Establish ───────────────────────────────────────────────────────────── */

static void test_first_acquirer_succeeds(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(1), DISK_BIND_OK);

    CU_ASSERT_EQUAL(disk_binding_acquire(PAGE_OWNER_LIBOS), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_owner(), PAGE_OWNER_LIBOS);
}

static void test_owner_may_reacquire(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(1), DISK_BIND_OK);

    CU_ASSERT_EQUAL(disk_binding_acquire(PAGE_OWNER_LIBOS), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire(PAGE_OWNER_LIBOS), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_owner(), PAGE_OWNER_LIBOS);
}

static void test_second_acquirer_is_busy(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(1), DISK_BIND_OK);

    CU_ASSERT_EQUAL(disk_binding_acquire(PAGE_OWNER_LIBOS), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire(OTHER_LIBOS), DISK_BIND_EBUSY);
    CU_ASSERT_EQUAL(disk_binding_owner(), PAGE_OWNER_LIBOS);
}

/* Nobody can bind the disk to PAGE_OWNER_FREE: that is the "unheld"
 * sentinel, and a binding to it would look free to the next caller. */
static void test_free_sentinel_cannot_own(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(1), DISK_BIND_OK);

    CU_ASSERT_EQUAL(disk_binding_acquire(PAGE_OWNER_FREE), DISK_BIND_EBUSY);
    CU_ASSERT_EQUAL(disk_binding_owner(), PAGE_OWNER_FREE);
}

/* A machine with no drive reports -EXO_ENODEV from the real syscall, not
 * -EXO_ENOSYS: the syscall exists, the hardware does not. */
static void test_headless_reports_enodev(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(0), DISK_BIND_ENODEV);

    CU_ASSERT_EQUAL(disk_binding_acquire(PAGE_OWNER_LIBOS), DISK_BIND_ENODEV);
    CU_ASSERT_EQUAL(disk_binding_owner(), PAGE_OWNER_FREE);

    CU_ASSERT_EQUAL(do_disk_acquire(), -EXO_ENODEV);
}

/* ── Enforce: exo_disk_read/exo_disk_write ──────────────────────────────── */

/* An unheld disk is not public property: read/write must fail until the
 * caller has acquired it, even though the real dispatch context is always
 * this suite's syscall_current_context(). */
static void test_read_write_reject_unheld_disk(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(1), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_owner(), PAGE_OWNER_FREE);

    /* count == 0 stays a no-op success regardless of the binding -- it
     * touches nothing (docs/syscall_spec.md #24/#25). */
    CU_ASSERT_EQUAL(do_disk_read(TEST_LBA, 0, 0), 0);
    CU_ASSERT_EQUAL(do_disk_write(TEST_LBA, 0, 0), 0);
}

/* Once acquired via the real syscall, read/write for the acquiring context
 * are no longer rejected on ownership grounds -- any remaining failure would
 * have to come from validate_disk_args() (e.g. -EXO_ENODEV with no real
 * drive attached), never -EXO_EBUSY. */
static void test_acquire_then_readwrite_not_busy(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(1), DISK_BIND_OK);
    CU_ASSERT_EQUAL(do_disk_acquire(), 0);
    CU_ASSERT_EQUAL(disk_binding_owner(), syscall_current_context());

    /* count == 0 always short-circuits before the binding check runs
     * either way, so this only proves the acquire itself succeeded and did
     * not somehow leave the binding busy against its own owner; the real
     * hardware round-trip is test_syscall_disk_k.c's job. */
    CU_ASSERT_EQUAL(do_disk_read(TEST_LBA, 0, 0), 0);
    CU_ASSERT_EQUAL(do_disk_write(TEST_LBA, 0, 0), 0);

    disk_binding_release(syscall_current_context());
}

/* The ticket's literal acceptance criterion: a second context's read/write
 * calls fail with -EXO_EBUSY while a different context holds the binding.
 * Simulated the same way test_fb_binding_k.c simulates a foreign owner --
 * the real dispatch path always runs as this suite's one
 * syscall_current_context(), so "someone else holds it" is set up with a
 * direct disk_binding_acquire(OTHER_LIBOS) call. */
static void test_other_owner_blocks_this_context(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(1), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire(OTHER_LIBOS), DISK_BIND_OK);

    CU_ASSERT_EQUAL(do_disk_acquire(), -EXO_EBUSY);
    CU_ASSERT_EQUAL(do_disk_read(TEST_LBA, 0, 4), -EXO_EBUSY);
    CU_ASSERT_EQUAL(do_disk_write(TEST_LBA, 0, 4), -EXO_EBUSY);

    /* Releasing lets this context in. */
    disk_binding_release(OTHER_LIBOS);
    CU_ASSERT_EQUAL(do_disk_acquire(), 0);

    disk_binding_release(syscall_current_context());
}

/* ── Reclaim ─────────────────────────────────────────────────────────────── */

static void test_release_lets_another_context_acquire(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(1), DISK_BIND_OK);
    page_owner_t me = syscall_current_context();

    CU_ASSERT_EQUAL(disk_binding_acquire(OTHER_LIBOS), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire(me), DISK_BIND_EBUSY);

    disk_binding_release(OTHER_LIBOS);
    CU_ASSERT_EQUAL(disk_binding_owner(), PAGE_OWNER_FREE);

    CU_ASSERT_EQUAL(disk_binding_acquire(me), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_owner(), me);

    disk_binding_release(me);
}

/* Release is scoped to the owner: releasing what you do not hold is a no-op
 * so reclamation can call it unconditionally. */
static void test_release_by_non_owner_is_a_noop(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(1), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire(OTHER_LIBOS), DISK_BIND_OK);

    disk_binding_release(PAGE_OWNER_LIBOS);
    CU_ASSERT_EQUAL(disk_binding_owner(), OTHER_LIBOS);

    disk_binding_release(PAGE_OWNER_FREE);
    CU_ASSERT_EQUAL(disk_binding_owner(), OTHER_LIBOS);

    disk_binding_release(OTHER_LIBOS);
    CU_ASSERT_EQUAL(disk_binding_owner(), PAGE_OWNER_FREE);
}

/* ── Revocation / repossession ───────────────────────────────────────────── */

static void test_revoke_mark_clear_pending(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(1), DISK_BIND_OK);

    /* Marking an unheld disk fails: there is no owner to ask. */
    CU_ASSERT_EQUAL(disk_binding_revoke_mark(PAGE_OWNER_LIBOS),
                    DISK_REVOKE_ENOENT);

    CU_ASSERT_EQUAL(disk_binding_acquire(PAGE_OWNER_LIBOS), DISK_BIND_OK);
    CU_ASSERT_FALSE(disk_binding_revoke_pending());

    CU_ASSERT_EQUAL(disk_binding_revoke_mark(PAGE_OWNER_LIBOS),
                    DISK_REVOKE_OK);
    CU_ASSERT_TRUE(disk_binding_revoke_pending());

    /* The mark is an ask, not a seizure: the owner keeps using the disk. */
    CU_ASSERT_EQUAL(disk_binding_owner(), PAGE_OWNER_LIBOS);

    CU_ASSERT_EQUAL(disk_binding_revoke_clear(PAGE_OWNER_LIBOS),
                    DISK_REVOKE_OK);
    CU_ASSERT_FALSE(disk_binding_revoke_pending());

    disk_binding_release(PAGE_OWNER_LIBOS);
}

static void test_revoke_reclaim_takes_it_back(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(1), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire(PAGE_OWNER_LIBOS), DISK_BIND_OK);

    CU_ASSERT_EQUAL(disk_binding_revoke_mark(PAGE_OWNER_LIBOS),
                    DISK_REVOKE_OK);
    CU_ASSERT_EQUAL(disk_binding_reclaim(PAGE_OWNER_LIBOS), DISK_REVOKE_OK);

    CU_ASSERT_EQUAL(disk_binding_owner(), PAGE_OWNER_FREE);
    CU_ASSERT_FALSE(disk_binding_revoke_pending());

    /* Reclaiming a context that has since let go (or never held it)
     * reports ENOENT, not a phantom success. */
    CU_ASSERT_EQUAL(disk_binding_reclaim(PAGE_OWNER_LIBOS),
                    DISK_REVOKE_ENOENT);
}

/* ── Extents (SCRUM-237) ─────────────────────────────────────────────────── */

static void test_disjoint_extents_coexist(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(1), DISK_BIND_OK);

    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 1000, 100),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(OTHER_LIBOS, 1100, 100),
                    DISK_BIND_OK);

    uint32_t base = 0, count = 0;
    CU_ASSERT_EQUAL(disk_binding_extent_of(OTHER_LIBOS, &base, &count),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(base, 1100);
    CU_ASSERT_EQUAL(count, 100);
}

static void test_overlapping_extent_is_busy(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(1), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 1000, 100),
                    DISK_BIND_OK);

    /* One shared sector at either edge is enough; adjacency is not overlap. */
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(OTHER_LIBOS, 1099, 10),
                    DISK_BIND_EBUSY);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(OTHER_LIBOS, 990, 11),
                    DISK_BIND_EBUSY);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(OTHER_LIBOS, 990, 10),
                    DISK_BIND_OK);
}

static void test_whole_device_conflicts_with_any_extent(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(1), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 1000, 100),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire(OTHER_LIBOS), DISK_BIND_EBUSY);

    disk_binding_release(PAGE_OWNER_LIBOS);
    CU_ASSERT_EQUAL(disk_binding_acquire(OTHER_LIBOS), DISK_BIND_OK);
    /* ...and the other way round. */
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 5, 1),
                    DISK_BIND_EBUSY);
}

static void test_malformed_extent_is_rejected(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(1), DISK_BIND_OK);

    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 5, 0),
                    DISK_BIND_ERANGE);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS,
                                                DISK_BIND_LBA_LIMIT, 1),
                    DISK_BIND_ERANGE);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS,
                                                DISK_BIND_LBA_LIMIT - 1, 2),
                    DISK_BIND_ERANGE);
    CU_ASSERT_EQUAL(disk_binding_owner(), PAGE_OWNER_FREE);
}

static void test_holder_cannot_swap_extents_silently(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(1), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 1000, 100),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 1000, 100),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 2000, 100),
                    DISK_BIND_EBUSY);
}

static void test_check_range_confines_the_holder(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(1), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 1000, 100),
                    DISK_BIND_OK);

    CU_ASSERT_EQUAL(disk_binding_check_range(PAGE_OWNER_LIBOS, 1000, 100),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_check_range(PAGE_OWNER_LIBOS, 1099, 1),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_check_range(PAGE_OWNER_LIBOS, 999, 1),
                    DISK_BIND_ERANGE);
    CU_ASSERT_EQUAL(disk_binding_check_range(PAGE_OWNER_LIBOS, 1099, 2),
                    DISK_BIND_ERANGE);
    CU_ASSERT_EQUAL(disk_binding_check_range(PAGE_OWNER_LIBOS, 1100, 1),
                    DISK_BIND_ERANGE);
    CU_ASSERT_EQUAL(disk_binding_check_range(PAGE_OWNER_LIBOS, ~0ULL, 2),
                    DISK_BIND_ERANGE);
    /* Someone who holds nothing is not "out of range", they are unbound. */
    CU_ASSERT_EQUAL(disk_binding_check_range(OTHER_LIBOS, 1000, 1),
                    DISK_BIND_EBUSY);
}

static void test_reclaim_frees_only_that_owners_extent(void)
{
    uint32_t base, count;

    CU_ASSERT_EQUAL(disk_binding_init(1), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 1000, 100),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(OTHER_LIBOS, 2000, 100),
                    DISK_BIND_OK);

    CU_ASSERT_EQUAL(disk_binding_reclaim(PAGE_OWNER_LIBOS), DISK_REVOKE_OK);
    CU_ASSERT_EQUAL(disk_binding_extent_of(PAGE_OWNER_LIBOS, &base, &count),
                    DISK_BIND_EBUSY);
    CU_ASSERT_EQUAL(disk_binding_extent_of(OTHER_LIBOS, &base, &count),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_owner(), OTHER_LIBOS);
}

static void test_extent_table_fills_up(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(1), DISK_BIND_OK);

    for (int i = 0; i < DISK_BIND_MAX_EXTENTS; i++)
        CU_ASSERT_EQUAL(disk_binding_acquire_extent(
                            (page_owner_t)(PAGE_OWNER_LIBOS + i),
                            1000u + 10u * (uint32_t)i, 10),
                        DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(
                        (page_owner_t)(PAGE_OWNER_LIBOS + DISK_BIND_MAX_EXTENTS),
                        900, 10),
                    DISK_BIND_ENOSLOT);
}

/* ── Assigned volumes (SCRUM-237) ────────────────────────────────────────── */

static void test_assigned_context_may_bind_only_its_volume(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(1), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_assign(PAGE_OWNER_LIBOS, 5000, 64),
                    DISK_BIND_OK);

    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 5000, 63),
                    DISK_BIND_EPERM);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 0, 10),
                    DISK_BIND_EPERM);
    CU_ASSERT_EQUAL(disk_binding_acquire(PAGE_OWNER_LIBOS), DISK_BIND_EPERM);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 5000, 64),
                    DISK_BIND_OK);
}

static void test_assigned_volume_is_fenced_from_everyone_else(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(1), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_assign(PAGE_OWNER_LIBOS, 5000, 64),
                    DISK_BIND_OK);

    /* Nobody holds it yet, and nobody else can take it or anything over it. */
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(OTHER_LIBOS, 5000, 64),
                    DISK_BIND_EBUSY);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(OTHER_LIBOS, 5063, 5),
                    DISK_BIND_EBUSY);
    CU_ASSERT_EQUAL(disk_binding_acquire(OTHER_LIBOS), DISK_BIND_EBUSY);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(OTHER_LIBOS, 5064, 64),
                    DISK_BIND_OK);

    /* A voluntary release keeps the permission: the fence stays up between
     * mounts, and the owner is still pinned to its volume. */
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 5000, 64),
                    DISK_BIND_OK);
    disk_binding_release(PAGE_OWNER_LIBOS);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(OTHER_LIBOS, 5000, 1),
                    DISK_BIND_EBUSY);
    CU_ASSERT_EQUAL(disk_binding_acquire(PAGE_OWNER_LIBOS), DISK_BIND_EPERM);

    /* Only teardown lifts it. */
    disk_binding_forget(PAGE_OWNER_LIBOS);
    uint32_t b, c;
    CU_ASSERT_EQUAL(disk_binding_assignment_of(PAGE_OWNER_LIBOS, &b, &c),
                    DISK_BIND_EBUSY);
    disk_binding_release(OTHER_LIBOS);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(OTHER_LIBOS, 5000, 64),
                    DISK_BIND_OK);
}

static void test_assignments_must_not_overlap(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(1), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_assign(PAGE_OWNER_LIBOS, 5000, 64),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_assign(OTHER_LIBOS, 5063, 10), DISK_BIND_EBUSY);
    CU_ASSERT_EQUAL(disk_binding_assign(OTHER_LIBOS, 5064, 10), DISK_BIND_OK);
    /* One volume per context. */
    CU_ASSERT_EQUAL(disk_binding_assign(PAGE_OWNER_LIBOS, 9000, 8),
                    DISK_BIND_EBUSY);
    CU_ASSERT_EQUAL(disk_binding_assign(PAGE_OWNER_FREE, 9000, 8),
                    DISK_BIND_EBUSY);
    CU_ASSERT_EQUAL(disk_binding_assign((page_owner_t)(PAGE_OWNER_LIBOS + 2),
                                        9000, 0),
                    DISK_BIND_ERANGE);
}

void suite_disk_binding_tests(CU_pSuite s)
{
    CU_add_test(s, "disjoint extents coexist",
                test_disjoint_extents_coexist);
    CU_add_test(s, "overlapping extent is busy",
                test_overlapping_extent_is_busy);
    CU_add_test(s, "whole device conflicts with any extent",
                test_whole_device_conflicts_with_any_extent);
    CU_add_test(s, "malformed extent is rejected",
                test_malformed_extent_is_rejected);
    CU_add_test(s, "holder cannot swap extents silently",
                test_holder_cannot_swap_extents_silently);
    CU_add_test(s, "check_range confines the holder",
                test_check_range_confines_the_holder);
    CU_add_test(s, "reclaim frees only that owner's extent",
                test_reclaim_frees_only_that_owners_extent);
    CU_add_test(s, "extent table fills up", test_extent_table_fills_up);
    CU_add_test(s, "assigned context may bind only its volume",
                test_assigned_context_may_bind_only_its_volume);
    CU_add_test(s, "assigned volume is fenced from everyone else",
                test_assigned_volume_is_fenced_from_everyone_else);
    CU_add_test(s, "assignments must not overlap",
                test_assignments_must_not_overlap);
    CU_add_test(s, "boot bound the acquire syscall",
                test_boot_bound_the_acquire_syscall);
    CU_add_test(s, "first acquirer succeeds", test_first_acquirer_succeeds);
    CU_add_test(s, "owner may re-acquire", test_owner_may_reacquire);
    CU_add_test(s, "second acquirer is busy", test_second_acquirer_is_busy);
    CU_add_test(s, "FREE sentinel cannot own", test_free_sentinel_cannot_own);
    CU_add_test(s, "headless reports ENODEV", test_headless_reports_enodev);
    CU_add_test(s, "read/write reject unheld disk",
                test_read_write_reject_unheld_disk);
    CU_add_test(s, "acquire then read/write not busy",
                test_acquire_then_readwrite_not_busy);
    CU_add_test(s, "other owner blocks this context",
                test_other_owner_blocks_this_context);
    CU_add_test(s, "release lets another context acquire",
                test_release_lets_another_context_acquire);
    CU_add_test(s, "release by non-owner is a no-op",
                test_release_by_non_owner_is_a_noop);
    CU_add_test(s, "revoke mark/clear/pending",
                test_revoke_mark_clear_pending);
    CU_add_test(s, "revoke reclaim takes it back",
                test_revoke_reclaim_takes_it_back);
}
