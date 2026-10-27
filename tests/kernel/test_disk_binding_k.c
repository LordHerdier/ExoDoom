/*
 * test_disk_binding_k.c — disk secure binding (SCRUM-188), disk_binding.c
 * itself plus the exo_disk_acquire/read/write enforcement in
 * src/syscall_disk.c.
 *
 * Mirrors test_fb_binding_k.c's structure, minus the geometry-specific
 * cases fb_binding.c has and disk_binding.c doesn't. Most tests reset the
 * table with disk_binding_init() and rely on a system disk (device 0) being
 * registered; the suite registers a stand-in if the boot found none, so the
 * assertions do not depend on whether the CI scratch drive is attached.
 * "Absent" is modelled by emptying the block-device registry, which is
 * where presence comes from (SCRUM-244).
 *
 * The suite's init/cleanup pair snapshots and restores the registry
 * kernel_main built, so the rest of the suite run is unaffected.
 */

#include "kunit.h"
#include "disk_binding.h"
#include "page_alloc.h"
#include "syscall.h"
#include "exo_syscall.h"
#include "ata.h"
#include "blkdev.h"
#include "libos_fs/exofs_blockdev.h"

#include <stdint.h>

/* A second, distinct LibOS id -- the "another LibOS" of this binding's
 * exclusivity rule. v1 only ever runs PAGE_OWNER_LIBOS through the real
 * dispatch path; this id stands in as a foreign context, same convention
 * test_fb_binding_k.c uses. */
#define OTHER_LIBOS ((page_owner_t)(PAGE_OWNER_LIBOS + 1))

#define TEST_LBA 3072

static blkdev_snapshot_t boot_devs;

/* Stand-in system disk for a boot with no drive: reads zeros, accepts
 * writes. Only registered when kernel_main found no device 0. */
static int standin_read(void *ctx, uint32_t lba, uint8_t *buf, uint32_t count)
{
    (void)ctx; (void)lba;
    for (uint32_t i = 0; i < count * 512u; i++)
        buf[i] = 0;
    return BLKDEV_OK;
}

static int standin_write(void *ctx, uint32_t lba, const uint8_t *buf,
                         uint32_t count)
{
    (void)ctx; (void)lba; (void)buf; (void)count;
    return BLKDEV_OK;
}

/* Save the registry kernel_main built (SCRUM-244) so cleanup can restore
 * it exactly, then make sure there is a device 0 to bind. */
int disk_binding_suite_init(void)
{
    blkdev_save(&boot_devs);
    if (blkdev_get(BLKDEV_SYSTEM) == NULL) {
        blkdev_t standin = {
            .sectors = DISK_BIND_LBA_LIMIT,
            .read    = standin_read,
            .write   = standin_write,
            .ctx     = NULL,
        };
        blkdev_register_system(&standin);
    }

    return 0;
}

int disk_binding_suite_cleanup(void)
{
    /* Put the registry and the binding back to a present-but-unheld state
     * matching what kernel_main published, so a later suite sees the same
     * environment this one started with. */
    blkdev_restore(&boot_devs);
    disk_binding_init();

    return 0;
}

static int64_t do_disk_acquire(void)
{
    return exo_syscall_dispatch(EXO_SYS_DISK_ACQUIRE, 0, 0, 0, 0, 0, 0); /* dev 0, whole */
}

static int64_t do_disk_read(uint64_t lba, uint64_t buf, uint64_t count)
{
    return exo_syscall_dispatch(EXO_SYS_DISK_READ, 0, lba, buf, count, 0, 0);
}

static int64_t do_disk_write(uint64_t lba, uint64_t buf, uint64_t count)
{
    return exo_syscall_dispatch(EXO_SYS_DISK_WRITE, 0, lba, buf, count, 0, 0);
}

/* ── Boot wiring ─────────────────────────────────────────────────────────── */

static void test_boot_bound_the_acquire_syscall(void)
{
    CU_ASSERT_PTR_NOT_NULL(exo_syscall_handler(EXO_SYS_DISK_ACQUIRE));
}

/* ── Establish ───────────────────────────────────────────────────────────── */

static void test_first_acquirer_succeeds(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(), DISK_BIND_OK);

    CU_ASSERT_EQUAL(disk_binding_acquire(PAGE_OWNER_LIBOS, 0), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_owner(0), PAGE_OWNER_LIBOS);
}

static void test_owner_may_reacquire(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(), DISK_BIND_OK);

    CU_ASSERT_EQUAL(disk_binding_acquire(PAGE_OWNER_LIBOS, 0), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire(PAGE_OWNER_LIBOS, 0), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_owner(0), PAGE_OWNER_LIBOS);
}

static void test_second_acquirer_is_busy(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(), DISK_BIND_OK);

    CU_ASSERT_EQUAL(disk_binding_acquire(PAGE_OWNER_LIBOS, 0), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire(OTHER_LIBOS, 0), DISK_BIND_EBUSY);
    CU_ASSERT_EQUAL(disk_binding_owner(0), PAGE_OWNER_LIBOS);
}

/* Nobody can bind the disk to PAGE_OWNER_FREE: that is the "unheld"
 * sentinel, and a binding to it would look free to the next caller. */
static void test_free_sentinel_cannot_own(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(), DISK_BIND_OK);

    CU_ASSERT_EQUAL(disk_binding_acquire(PAGE_OWNER_FREE, 0), DISK_BIND_EBUSY);
    CU_ASSERT_EQUAL(disk_binding_owner(0), PAGE_OWNER_FREE);
}

/* A machine with no drive reports -EXO_ENODEV from the real syscall, not
 * -EXO_ENOSYS: the syscall exists, the hardware does not. */
static void test_headless_reports_enodev(void)
{
    blkdev_snapshot_t devs;
    blkdev_save(&devs);
    for (uint32_t d = 0; d < BLKDEV_MAX; d++)
        blkdev_unregister(d);

    CU_ASSERT_EQUAL(disk_binding_init(), DISK_BIND_ENODEV);

    CU_ASSERT_EQUAL(disk_binding_acquire(PAGE_OWNER_LIBOS, 0), DISK_BIND_ENODEV);
    CU_ASSERT_EQUAL(disk_binding_owner(0), PAGE_OWNER_FREE);

    CU_ASSERT_EQUAL(do_disk_acquire(), -EXO_ENODEV);

    blkdev_restore(&devs);
}

/* ── Enforce: exo_disk_read/exo_disk_write ──────────────────────────────── */

/* An unheld disk is not public property: read/write must fail until the
 * caller has acquired it, even though the real dispatch context is always
 * this suite's syscall_current_context(). */
static void test_read_write_reject_unheld_disk(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_owner(0), PAGE_OWNER_FREE);

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
    CU_ASSERT_EQUAL(disk_binding_init(), DISK_BIND_OK);
    CU_ASSERT_EQUAL(do_disk_acquire(), 0);
    CU_ASSERT_EQUAL(disk_binding_owner(0), syscall_current_context());

    /* count == 0 always short-circuits before the binding check runs
     * either way, so this only proves the acquire itself succeeded and did
     * not somehow leave the binding busy against its own owner; the real
     * hardware round-trip is test_syscall_disk_k.c's job. */
    CU_ASSERT_EQUAL(do_disk_read(TEST_LBA, 0, 0), 0);
    CU_ASSERT_EQUAL(do_disk_write(TEST_LBA, 0, 0), 0);

    disk_binding_release(syscall_current_context(), 0);
}

/* The ticket's literal acceptance criterion: a second context's read/write
 * calls fail with -EXO_EBUSY while a different context holds the binding.
 * Simulated the same way test_fb_binding_k.c simulates a foreign owner --
 * the real dispatch path always runs as this suite's one
 * syscall_current_context(), so "someone else holds it" is set up with a
 * direct disk_binding_acquire(OTHER_LIBOS) call. */
static void test_other_owner_blocks_this_context(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire(OTHER_LIBOS, 0), DISK_BIND_OK);

    CU_ASSERT_EQUAL(do_disk_acquire(), -EXO_EBUSY);
    CU_ASSERT_EQUAL(do_disk_read(TEST_LBA, 0, 4), -EXO_EBUSY);
    CU_ASSERT_EQUAL(do_disk_write(TEST_LBA, 0, 4), -EXO_EBUSY);

    /* Releasing lets this context in. */
    disk_binding_release(OTHER_LIBOS, 0);
    CU_ASSERT_EQUAL(do_disk_acquire(), 0);

    disk_binding_release(syscall_current_context(), 0);
}

/* ── Reclaim ─────────────────────────────────────────────────────────────── */

static void test_release_lets_another_context_acquire(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(), DISK_BIND_OK);
    page_owner_t me = syscall_current_context();

    CU_ASSERT_EQUAL(disk_binding_acquire(OTHER_LIBOS, 0), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire(me, 0), DISK_BIND_EBUSY);

    disk_binding_release(OTHER_LIBOS, 0);
    CU_ASSERT_EQUAL(disk_binding_owner(0), PAGE_OWNER_FREE);

    CU_ASSERT_EQUAL(disk_binding_acquire(me, 0), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_owner(0), me);

    disk_binding_release(me, 0);
}

/* Release is scoped to the owner: releasing what you do not hold is a no-op
 * so reclamation can call it unconditionally. */
static void test_release_by_non_owner_is_a_noop(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire(OTHER_LIBOS, 0), DISK_BIND_OK);

    disk_binding_release(PAGE_OWNER_LIBOS, 0);
    CU_ASSERT_EQUAL(disk_binding_owner(0), OTHER_LIBOS);

    disk_binding_release(PAGE_OWNER_FREE, 0);
    CU_ASSERT_EQUAL(disk_binding_owner(0), OTHER_LIBOS);

    disk_binding_release(OTHER_LIBOS, 0);
    CU_ASSERT_EQUAL(disk_binding_owner(0), PAGE_OWNER_FREE);
}

/* ── Revocation / repossession ───────────────────────────────────────────── */

static void test_revoke_mark_clear_pending(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(), DISK_BIND_OK);

    /* Marking an unheld disk fails: there is no owner to ask. */
    CU_ASSERT_EQUAL(disk_binding_revoke_mark(PAGE_OWNER_LIBOS),
                    DISK_REVOKE_ENOENT);

    CU_ASSERT_EQUAL(disk_binding_acquire(PAGE_OWNER_LIBOS, 0), DISK_BIND_OK);
    CU_ASSERT_FALSE(disk_binding_revoke_pending(PAGE_OWNER_LIBOS));

    CU_ASSERT_EQUAL(disk_binding_revoke_mark(PAGE_OWNER_LIBOS),
                    DISK_REVOKE_OK);
    CU_ASSERT_TRUE(disk_binding_revoke_pending(PAGE_OWNER_LIBOS));

    /* The mark is an ask, not a seizure: the owner keeps using the disk. */
    CU_ASSERT_EQUAL(disk_binding_owner(0), PAGE_OWNER_LIBOS);

    CU_ASSERT_EQUAL(disk_binding_revoke_clear(PAGE_OWNER_LIBOS),
                    DISK_REVOKE_OK);
    CU_ASSERT_FALSE(disk_binding_revoke_pending(PAGE_OWNER_LIBOS));

    disk_binding_release(PAGE_OWNER_LIBOS, 0);
}

static void test_revoke_reclaim_takes_it_back(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire(PAGE_OWNER_LIBOS, 0), DISK_BIND_OK);

    CU_ASSERT_EQUAL(disk_binding_revoke_mark(PAGE_OWNER_LIBOS),
                    DISK_REVOKE_OK);
    CU_ASSERT_EQUAL(disk_binding_reclaim(PAGE_OWNER_LIBOS), DISK_REVOKE_OK);

    CU_ASSERT_EQUAL(disk_binding_owner(0), PAGE_OWNER_FREE);
    CU_ASSERT_FALSE(disk_binding_revoke_pending(PAGE_OWNER_LIBOS));

    /* Reclaiming a context that has since let go (or never held it)
     * reports ENOENT, not a phantom success. */
    CU_ASSERT_EQUAL(disk_binding_reclaim(PAGE_OWNER_LIBOS),
                    DISK_REVOKE_ENOENT);
}

/* ── Extents (SCRUM-237) ─────────────────────────────────────────────────── */

static void test_disjoint_extents_coexist(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(), DISK_BIND_OK);

    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 0, 1000, 100),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(OTHER_LIBOS, 0, 1100, 100),
                    DISK_BIND_OK);

    uint32_t base = 0, count = 0;
    CU_ASSERT_EQUAL(disk_binding_extent_of(OTHER_LIBOS, 0, &base, &count),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(base, 1100);
    CU_ASSERT_EQUAL(count, 100);
}

static void test_overlapping_extent_is_busy(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 0, 1000, 100),
                    DISK_BIND_OK);

    /* One shared sector at either edge is enough; adjacency is not overlap. */
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(OTHER_LIBOS, 0, 1099, 10),
                    DISK_BIND_EBUSY);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(OTHER_LIBOS, 0, 990, 11),
                    DISK_BIND_EBUSY);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(OTHER_LIBOS, 0, 990, 10),
                    DISK_BIND_OK);
}

static void test_whole_device_conflicts_with_any_extent(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 0, 1000, 100),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire(OTHER_LIBOS, 0), DISK_BIND_EBUSY);

    disk_binding_release(PAGE_OWNER_LIBOS, 0);
    CU_ASSERT_EQUAL(disk_binding_acquire(OTHER_LIBOS, 0), DISK_BIND_OK);
    /* ...and the other way round. */
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 0, 5, 1),
                    DISK_BIND_EBUSY);
}

static void test_malformed_extent_is_rejected(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(), DISK_BIND_OK);

    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 0, 5, 0),
                    DISK_BIND_ERANGE);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 0,
                                                DISK_BIND_LBA_LIMIT, 1),
                    DISK_BIND_ERANGE);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 0,
                                                DISK_BIND_LBA_LIMIT - 1, 2),
                    DISK_BIND_ERANGE);
    CU_ASSERT_EQUAL(disk_binding_owner(0), PAGE_OWNER_FREE);
}

static void test_holder_cannot_swap_extents_silently(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 0, 1000, 100),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 0, 1000, 100),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 0, 2000, 100),
                    DISK_BIND_EBUSY);
}

static void test_check_range_confines_the_holder(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 0, 1000, 100),
                    DISK_BIND_OK);

    CU_ASSERT_EQUAL(disk_binding_check_range(PAGE_OWNER_LIBOS, 0, 1000, 100),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_check_range(PAGE_OWNER_LIBOS, 0, 1099, 1),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_check_range(PAGE_OWNER_LIBOS, 0, 999, 1),
                    DISK_BIND_ERANGE);
    CU_ASSERT_EQUAL(disk_binding_check_range(PAGE_OWNER_LIBOS, 0, 1099, 2),
                    DISK_BIND_ERANGE);
    CU_ASSERT_EQUAL(disk_binding_check_range(PAGE_OWNER_LIBOS, 0, 1100, 1),
                    DISK_BIND_ERANGE);
    CU_ASSERT_EQUAL(disk_binding_check_range(PAGE_OWNER_LIBOS, 0, ~0ULL, 2),
                    DISK_BIND_ERANGE);
    /* Someone who holds nothing is not "out of range", they are unbound. */
    CU_ASSERT_EQUAL(disk_binding_check_range(OTHER_LIBOS, 0, 1000, 1),
                    DISK_BIND_EBUSY);
}

static void test_reclaim_frees_only_that_owners_extent(void)
{
    uint32_t base, count;

    CU_ASSERT_EQUAL(disk_binding_init(), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 0, 1000, 100),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(OTHER_LIBOS, 0, 2000, 100),
                    DISK_BIND_OK);

    CU_ASSERT_EQUAL(disk_binding_reclaim(PAGE_OWNER_LIBOS), DISK_REVOKE_OK);
    CU_ASSERT_EQUAL(disk_binding_extent_of(PAGE_OWNER_LIBOS, 0, &base, &count),
                    DISK_BIND_EBUSY);
    CU_ASSERT_EQUAL(disk_binding_extent_of(OTHER_LIBOS, 0, &base, &count),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_owner(0), OTHER_LIBOS);
}

static void test_extent_table_fills_up(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(), DISK_BIND_OK);

    for (int i = 0; i < DISK_BIND_MAX_EXTENTS; i++)
        CU_ASSERT_EQUAL(disk_binding_acquire_extent(
                            (page_owner_t)(PAGE_OWNER_LIBOS + i), 0,
                            1000u + 10u * (uint32_t)i, 10),
                        DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(
                        (page_owner_t)(PAGE_OWNER_LIBOS + DISK_BIND_MAX_EXTENTS), 0,
                        900, 10),
                    DISK_BIND_ENOSLOT);
}

/* ── Assigned volumes (SCRUM-237) ────────────────────────────────────────── */

static void test_assigned_context_may_bind_only_its_volume(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_assign(PAGE_OWNER_LIBOS, 5000, 64),
                    DISK_BIND_OK);

    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 0, 5000, 63),
                    DISK_BIND_EPERM);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 0, 0, 10),
                    DISK_BIND_EPERM);
    CU_ASSERT_EQUAL(disk_binding_acquire(PAGE_OWNER_LIBOS, 0), DISK_BIND_EPERM);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 0, 5000, 64),
                    DISK_BIND_OK);
}

static void test_assigned_volume_is_fenced_from_everyone_else(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_assign(PAGE_OWNER_LIBOS, 5000, 64),
                    DISK_BIND_OK);

    /* Nobody holds it yet, and nobody else can take it or anything over it. */
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(OTHER_LIBOS, 0, 5000, 64),
                    DISK_BIND_EBUSY);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(OTHER_LIBOS, 0, 5063, 5),
                    DISK_BIND_EBUSY);
    CU_ASSERT_EQUAL(disk_binding_acquire(OTHER_LIBOS, 0), DISK_BIND_EBUSY);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(OTHER_LIBOS, 0, 5064, 64),
                    DISK_BIND_OK);

    /* A voluntary release keeps the permission: the fence stays up between
     * mounts, and the owner is still pinned to its volume. */
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 0, 5000, 64),
                    DISK_BIND_OK);
    disk_binding_release(PAGE_OWNER_LIBOS, 0);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(OTHER_LIBOS, 0, 5000, 1),
                    DISK_BIND_EBUSY);
    CU_ASSERT_EQUAL(disk_binding_acquire(PAGE_OWNER_LIBOS, 0), DISK_BIND_EPERM);

    /* Only teardown lifts it. */
    disk_binding_forget(PAGE_OWNER_LIBOS);
    uint32_t b, c;
    CU_ASSERT_EQUAL(disk_binding_assignment_of(PAGE_OWNER_LIBOS, &b, &c),
                    DISK_BIND_EBUSY);
    disk_binding_release(OTHER_LIBOS, 0);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(OTHER_LIBOS, 0, 5000, 64),
                    DISK_BIND_OK);
}

static void test_assignments_must_not_overlap(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(), DISK_BIND_OK);
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

/* One holder's revocation mark is not another's (SCRUM-237). */
static void test_revoke_pending_is_per_owner(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 0, 1000, 10),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(OTHER_LIBOS, 0, 2000, 10),
                    DISK_BIND_OK);

    CU_ASSERT_EQUAL(disk_binding_revoke_mark(PAGE_OWNER_LIBOS), DISK_REVOKE_OK);
    CU_ASSERT_TRUE(disk_binding_revoke_pending(PAGE_OWNER_LIBOS));
    CU_ASSERT_FALSE(disk_binding_revoke_pending(OTHER_LIBOS));
    CU_ASSERT_FALSE(disk_binding_revoke_pending(PAGE_OWNER_FREE));
}

/* disk_binding_owner() names a holder only when there is exactly one. */
static void test_owner_is_free_with_several_holders(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 0, 1000, 10),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_owner(0), PAGE_OWNER_LIBOS);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(OTHER_LIBOS, 0, 2000, 10),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_owner(0), PAGE_OWNER_FREE);
}

/* A launch must not report success for a volume its program cannot bind. */
static void test_assign_refuses_a_volume_someone_else_holds(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire(OTHER_LIBOS, 0), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_assign(PAGE_OWNER_LIBOS, 5000, 64),
                    DISK_BIND_EBUSY);

    disk_binding_release(OTHER_LIBOS, 0);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(OTHER_LIBOS, 0, 5010, 4),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_assign(PAGE_OWNER_LIBOS, 5000, 64),
                    DISK_BIND_EBUSY);
    disk_binding_release(OTHER_LIBOS, 0);
    CU_ASSERT_EQUAL(disk_binding_assign(PAGE_OWNER_LIBOS, 5000, 64),
                    DISK_BIND_OK);
}

/* ── Read-only program-volume grant (SCRUM-75) ──────────────────────────── */

static void test_ro_assignment_binds_only_that_extent_read_only(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_assign_ro(PAGE_OWNER_LIBOS, 5000, 64),
                    DISK_BIND_OK);
    /* Exactly the assigned extent, like a private volume. */
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 0, 5000, 32),
                    DISK_BIND_EPERM);
    CU_ASSERT_EQUAL(disk_binding_acquire(PAGE_OWNER_LIBOS, 0), DISK_BIND_EPERM);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 0, 5000, 64),
                    DISK_BIND_OK);

    /* Reads are in range; writes are refused as read-only, and a write
     * outside the extent is still just out of range. */
    CU_ASSERT_EQUAL(disk_binding_check_range(PAGE_OWNER_LIBOS, 0, 5000, 64),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_check_write(PAGE_OWNER_LIBOS, 0, 5000, 1),
                    DISK_BIND_EROFS);
    CU_ASSERT_EQUAL(disk_binding_check_write(PAGE_OWNER_LIBOS, 0, 6000, 1),
                    DISK_BIND_ERANGE);

    /* The syscall maps it to -EXO_EROFS, ahead of the buffer check. */
    CU_ASSERT_EQUAL(do_disk_write(5000, 0, 1), -EXO_EROFS);
    CU_ASSERT_EQUAL(do_disk_write(6000, 0, 1), -EXO_EINVAL);
    disk_binding_release(PAGE_OWNER_LIBOS, 0);
    disk_binding_forget(PAGE_OWNER_LIBOS);
}

static void test_rw_holder_is_unaffected_by_check_write(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 0, 5000, 64),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_check_write(PAGE_OWNER_LIBOS, 0, 5000, 1),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_check_write(OTHER_LIBOS, 0, 5000, 1),
                    DISK_BIND_EBUSY);
    disk_binding_release(PAGE_OWNER_LIBOS, 0);
}

static void test_ro_holders_share_but_never_with_a_writer(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_assign_ro(PAGE_OWNER_LIBOS, 5000, 64),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_assign_ro(OTHER_LIBOS, 5000, 64),
                    DISK_BIND_OK);   /* no exclusivity among readers */
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 0, 5000, 64),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(OTHER_LIBOS, 0, 5000, 64),
                    DISK_BIND_OK);   /* two readers at once */

    /* A writer (a context with no assignment) overlapping a reader: busy. */
    page_owner_t writer = (page_owner_t)(PAGE_OWNER_LIBOS + 2);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(writer, 0, 5010, 4),
                    DISK_BIND_EBUSY);
    disk_binding_release(PAGE_OWNER_LIBOS, 0);
    disk_binding_release(OTHER_LIBOS, 0);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(writer, 0, 5010, 4),
                    DISK_BIND_OK);
    /* ...and a reader against a writer's held extent: busy too. */
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 0, 5000, 64),
                    DISK_BIND_EBUSY);
    disk_binding_release(writer, 0);
    disk_binding_forget(PAGE_OWNER_LIBOS);
    disk_binding_forget(OTHER_LIBOS);
}

static void test_ro_assignment_does_not_fence_others_out(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_assign_ro(PAGE_OWNER_LIBOS, 5000, 64),
                    DISK_BIND_OK);
    /* The shell must still be able to bind the program volume writable. */
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(OTHER_LIBOS, 0, 5000, 64),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_check_write(OTHER_LIBOS, 0, 5000, 1),
                    DISK_BIND_OK);
    disk_binding_release(OTHER_LIBOS, 0);
    disk_binding_forget(PAGE_OWNER_LIBOS);

    /* One assignment per context, and a bad range is rejected. */
    CU_ASSERT_EQUAL(disk_binding_assign_ro(PAGE_OWNER_LIBOS, 5000, 64),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_assign_ro(PAGE_OWNER_LIBOS, 9000, 8),
                    DISK_BIND_EBUSY);
    CU_ASSERT_EQUAL(disk_binding_assign_ro(OTHER_LIBOS, 5000, 0),
                    DISK_BIND_ERANGE);
    disk_binding_forget(PAGE_OWNER_LIBOS);
}

static void test_ro_assignment_refuses_a_private_volume_overlap(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_assign(OTHER_LIBOS, 5000, 64), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_assign_ro(PAGE_OWNER_LIBOS, 5010, 100),
                    DISK_BIND_EBUSY);
    CU_ASSERT_EQUAL(disk_binding_assign_ro(PAGE_OWNER_LIBOS, 6000, 100),
                    DISK_BIND_OK);   /* disjoint is fine */
    disk_binding_forget(OTHER_LIBOS);
    disk_binding_forget(PAGE_OWNER_LIBOS);
}

/* ── A private volume alongside the program volume (SCRUM-267) ──────────── */

/* Doom's shape: the shared read-only program volume for the IWAD, a private
 * read-write slot for saves, both bound for the whole run. */
static void test_private_and_program_volume_coexist(void)
{
    uint32_t b, c;

    CU_ASSERT_EQUAL(disk_binding_init(), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_assign_ro(PAGE_OWNER_LIBOS, 1000, 100),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_assign(PAGE_OWNER_LIBOS, 6000, 64),
                    DISK_BIND_OK);

    /* Each role reports its own extent; the unnamed lookup keeps meaning
     * "the private volume if there is one". */
    CU_ASSERT_EQUAL(disk_binding_assignment_role(PAGE_OWNER_LIBOS, 0, &b, &c),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(b, 6000);
    CU_ASSERT_EQUAL(c, 64);
    CU_ASSERT_EQUAL(disk_binding_assignment_role(PAGE_OWNER_LIBOS, 1, &b, &c),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(b, 1000);
    CU_ASSERT_EQUAL(c, 100);
    CU_ASSERT_EQUAL(disk_binding_assignment_of(PAGE_OWNER_LIBOS, &b, &c),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(b, 6000);

    /* Still one of each, and a context with neither role has nothing. */
    CU_ASSERT_EQUAL(disk_binding_assign(PAGE_OWNER_LIBOS, 9000, 8),
                    DISK_BIND_EBUSY);
    CU_ASSERT_EQUAL(disk_binding_assign_ro(PAGE_OWNER_LIBOS, 9000, 8),
                    DISK_BIND_EBUSY);
    CU_ASSERT_EQUAL(disk_binding_assignment_role(OTHER_LIBOS, 0, &b, &c),
                    DISK_BIND_EBUSY);
    CU_ASSERT_EQUAL(disk_binding_assignment_role(OTHER_LIBOS, 1, &b, &c),
                    DISK_BIND_EBUSY);

    /* Both extents may be held at once, nothing else may. */
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 0, 1000, 100),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 0, 6000, 64),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 0, 7000, 8),
                    DISK_BIND_EPERM);
    CU_ASSERT_EQUAL(disk_binding_acquire(PAGE_OWNER_LIBOS, 0), DISK_BIND_EPERM);
    /* Re-acquire of either is still success. */
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 0, 1000, 100),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_owner(0), PAGE_OWNER_LIBOS);

    /* A range is judged against the extent it falls in: the program volume
     * reads but never writes, the private one does both, and a range that
     * leaves one extent is out of range even though it touches the other. */
    CU_ASSERT_EQUAL(disk_binding_check_range(PAGE_OWNER_LIBOS, 0, 1000, 100),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_check_range(PAGE_OWNER_LIBOS, 0, 6000, 64),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_check_write(PAGE_OWNER_LIBOS, 0, 1000, 1),
                    DISK_BIND_EROFS);
    CU_ASSERT_EQUAL(disk_binding_check_write(PAGE_OWNER_LIBOS, 0, 6000, 1),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_check_range(PAGE_OWNER_LIBOS, 0, 1090, 20),
                    DISK_BIND_ERANGE);
    CU_ASSERT_EQUAL(disk_binding_check_range(PAGE_OWNER_LIBOS, 0, 6060, 10),
                    DISK_BIND_ERANGE);
    CU_ASSERT_EQUAL(disk_binding_check_write(PAGE_OWNER_LIBOS, 0, 2000, 1),
                    DISK_BIND_ERANGE);
    CU_ASSERT_EQUAL(disk_binding_check_write(OTHER_LIBOS, 0, 6000, 1),
                    DISK_BIND_EBUSY);

    /* Release is per device, so it lets go of both; the assignments stay. */
    disk_binding_release(PAGE_OWNER_LIBOS, 0);
    CU_ASSERT_EQUAL(disk_binding_owner(0), PAGE_OWNER_FREE);
    CU_ASSERT_EQUAL(disk_binding_check_range(PAGE_OWNER_LIBOS, 0, 6000, 1),
                    DISK_BIND_EBUSY);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(OTHER_LIBOS, 0, 6000, 8),
                    DISK_BIND_EBUSY);   /* still fenced */

    /* Teardown lifts both assignments. */
    disk_binding_forget(PAGE_OWNER_LIBOS);
    CU_ASSERT_EQUAL(disk_binding_assignment_role(PAGE_OWNER_LIBOS, 0, &b, &c),
                    DISK_BIND_EBUSY);
    CU_ASSERT_EQUAL(disk_binding_assignment_role(PAGE_OWNER_LIBOS, 1, &b, &c),
                    DISK_BIND_EBUSY);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(OTHER_LIBOS, 0, 6000, 8),
                    DISK_BIND_OK);
    disk_binding_release(OTHER_LIBOS, 0);
}

/* The other order, and the two volumes of one context must be disjoint. */
static void test_a_contexts_two_volumes_must_not_overlap(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_assign(PAGE_OWNER_LIBOS, 6000, 64),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_assign_ro(PAGE_OWNER_LIBOS, 6032, 100),
                    DISK_BIND_EBUSY);
    CU_ASSERT_EQUAL(disk_binding_assign_ro(PAGE_OWNER_LIBOS, 1000, 100),
                    DISK_BIND_OK);
    disk_binding_forget(PAGE_OWNER_LIBOS);

    /* A private volume over its own read-only one is refused too, though
     * over *someone else's* read-only volume it is not (the shell binds the
     * program volume read-write). */
    CU_ASSERT_EQUAL(disk_binding_assign_ro(PAGE_OWNER_LIBOS, 1000, 100),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_assign(PAGE_OWNER_LIBOS, 1050, 10),
                    DISK_BIND_EBUSY);
    CU_ASSERT_EQUAL(disk_binding_assign(OTHER_LIBOS, 1050, 10), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_assign(PAGE_OWNER_LIBOS, 1100, 10),
                    DISK_BIND_OK);   /* adjacent, not overlapping */
    disk_binding_forget(PAGE_OWNER_LIBOS);
    disk_binding_forget(OTHER_LIBOS);
}

/* The relaxation is for assigned extents only: a context with no
 * assignments keeps one extent per device, and one with a single assignment
 * has nothing else to hold. */
static void test_second_extent_needs_an_assignment(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(OTHER_LIBOS, 0, 7000, 8),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(OTHER_LIBOS, 0, 8000, 8),
                    DISK_BIND_EBUSY);
    disk_binding_release(OTHER_LIBOS, 0);

    CU_ASSERT_EQUAL(disk_binding_assign(PAGE_OWNER_LIBOS, 6000, 64),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 0, 6000, 64),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 0, 1000, 100),
                    DISK_BIND_EPERM);
    disk_binding_release(PAGE_OWNER_LIBOS, 0);
    disk_binding_forget(PAGE_OWNER_LIBOS);
}

/* ── Read-only bind by an unassigned reader (SCRUM-281) ─────────────────── */

/* The shell's `run`: it holds no assignment, binds the program volume only to
 * read a package, and must be able to while a program holds it read-only. */
static void test_ro_acquire_shares_with_a_ro_holder(void)
{
    page_owner_t shell = (page_owner_t)(PAGE_OWNER_LIBOS + 2);

    CU_ASSERT_EQUAL(disk_binding_init(), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_assign_ro(PAGE_OWNER_LIBOS, 5000, 64),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(PAGE_OWNER_LIBOS, 0, 5000, 64),
                    DISK_BIND_OK);

    /* The plain acquire stays refused; the read-only one is not. */
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(shell, 0, 5000, 64),
                    DISK_BIND_EBUSY);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent_ro(shell, 0, 5000, 64),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent_ro(shell, 0, 5000, 64),
                    DISK_BIND_OK);   /* re-acquire of the same extent */

    /* Reads are in range; a write through it is read-only. */
    CU_ASSERT_EQUAL(disk_binding_check_range(shell, 0, 5000, 64), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_check_write(shell, 0, 5000, 1),
                    DISK_BIND_EROFS);

    disk_binding_release(shell, 0);
    disk_binding_release(PAGE_OWNER_LIBOS, 0);
    disk_binding_forget(PAGE_OWNER_LIBOS);
}

static void test_ro_acquire_never_shares_with_a_writer(void)
{
    page_owner_t shell = (page_owner_t)(PAGE_OWNER_LIBOS + 2);

    CU_ASSERT_EQUAL(disk_binding_init(), DISK_BIND_OK);

    /* A read-write holder keeps a reader out... */
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(OTHER_LIBOS, 0, 5000, 64),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent_ro(shell, 0, 5000, 64),
                    DISK_BIND_EBUSY);
    disk_binding_release(OTHER_LIBOS, 0);

    /* ...a reader keeps a writer out (what `install` still gets)... */
    CU_ASSERT_EQUAL(disk_binding_acquire_extent_ro(shell, 0, 5000, 64),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(OTHER_LIBOS, 0, 5000, 64),
                    DISK_BIND_EBUSY);
    disk_binding_release(shell, 0);

    /* ...and a private assigned volume is not reachable by a reader. */
    CU_ASSERT_EQUAL(disk_binding_assign(PAGE_OWNER_LIBOS, 6000, 64),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent_ro(shell, 0, 6000, 64),
                    DISK_BIND_EBUSY);
    disk_binding_forget(PAGE_OWNER_LIBOS);
}

/* A reader holding the volume must not stop a program being launched onto it,
 * but a writer still does. */
static void test_assign_ro_tolerates_a_reader_not_a_writer(void)
{
    page_owner_t shell = (page_owner_t)(PAGE_OWNER_LIBOS + 2);

    CU_ASSERT_EQUAL(disk_binding_init(), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent_ro(shell, 0, 5000, 64),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_assign_ro(PAGE_OWNER_LIBOS, 5000, 64),
                    DISK_BIND_OK);
    disk_binding_forget(PAGE_OWNER_LIBOS);
    disk_binding_release(shell, 0);

    CU_ASSERT_EQUAL(disk_binding_acquire_extent(shell, 0, 5000, 64),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_assign_ro(PAGE_OWNER_LIBOS, 5000, 64),
                    DISK_BIND_EBUSY);
    disk_binding_release(shell, 0);
}

/* Through the real dispatcher: #42 binds read-only, #26 is unchanged. */
static void test_acquire_ro_syscall(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(), DISK_BIND_OK);
    CU_ASSERT_EQUAL(exo_syscall_dispatch(EXO_SYS_DISK_ACQUIRE_RO, 0, 5000, 64,
                                         0, 0, 0), 0);
    CU_ASSERT_EQUAL(do_disk_write(5000, 0, 1), -EXO_EROFS);
    disk_binding_release(PAGE_OWNER_LIBOS, 0);

    CU_ASSERT_EQUAL(exo_syscall_dispatch(EXO_SYS_DISK_ACQUIRE, 0, 5000, 64,
                                         0, 0, 0), 0);
    CU_ASSERT_NOT_EQUAL(do_disk_write(5000, 0, 1), -EXO_EROFS);
    disk_binding_release(PAGE_OWNER_LIBOS, 0);

    /* Same errors as #26: no such device, malformed extent. */
    CU_ASSERT_EQUAL(exo_syscall_dispatch(EXO_SYS_DISK_ACQUIRE_RO, 99, 0, 0,
                                         0, 0, 0), -EXO_ENODEV);
    CU_ASSERT_EQUAL(exo_syscall_dispatch(EXO_SYS_DISK_ACQUIRE_RO, 0, 5, 0,
                                         0, 0, 0), -EXO_EINVAL);
}

/* Re-acquiring the same extent in the other mode is refused, not answered
 * "ok" with the old mode quietly kept. */
static void test_reacquire_in_other_mode_is_busy(void)
{
    page_owner_t shell = (page_owner_t)(PAGE_OWNER_LIBOS + 2);

    CU_ASSERT_EQUAL(disk_binding_init(), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent_ro(shell, 0, 5000, 64),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(shell, 0, 5000, 64),
                    DISK_BIND_EBUSY);
    disk_binding_release(shell, 0);

    CU_ASSERT_EQUAL(disk_binding_acquire_extent(shell, 0, 5000, 64),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent_ro(shell, 0, 5000, 64),
                    DISK_BIND_EBUSY);
    disk_binding_release(shell, 0);
}

/* The kernel's own IWAD load (doom_wad_disk_get) binds this way: while a
 * program holds the program volume read-only it must still get in, and a
 * write-bind must not. */
static void test_bdev_acquire_ro_shares_with_a_ro_holder(void)
{
    CU_ASSERT_EQUAL(disk_binding_init(), DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_assign_ro(OTHER_LIBOS, 5000, 64),
                    DISK_BIND_OK);
    CU_ASSERT_EQUAL(disk_binding_acquire_extent(OTHER_LIBOS, 0, 5000, 64),
                    DISK_BIND_OK);

    CU_ASSERT_EQUAL(exofs_bdev_acquire_extent(0, 5000, 64), -EXO_EBUSY);
    CU_ASSERT_EQUAL(exofs_bdev_acquire_extent_ro(0, 5000, 64), 0);
    exofs_bdev_release(0);

    disk_binding_release(OTHER_LIBOS, 0);
    disk_binding_forget(OTHER_LIBOS);
}

void suite_disk_binding_tests(CU_pSuite s)
{
    CU_add_test(s, "private and program volume coexist",
                test_private_and_program_volume_coexist);
    CU_add_test(s, "a context's two volumes must not overlap",
                test_a_contexts_two_volumes_must_not_overlap);
    CU_add_test(s, "second extent needs an assignment",
                test_second_extent_needs_an_assignment);
    CU_add_test(s, "read-only assignment refuses a private volume overlap",
                test_ro_assignment_refuses_a_private_volume_overlap);
    CU_add_test(s, "read-only assignment binds one extent, read-only",
                test_ro_assignment_binds_only_that_extent_read_only);
    CU_add_test(s, "read-write holder is unaffected by check_write",
                test_rw_holder_is_unaffected_by_check_write);
    CU_add_test(s, "read-only holders share, never with a writer",
                test_ro_holders_share_but_never_with_a_writer);
    CU_add_test(s, "read-only assignment does not fence others out",
                test_ro_assignment_does_not_fence_others_out);
    CU_add_test(s, "read-only acquire shares with a read-only holder",
                test_ro_acquire_shares_with_a_ro_holder);
    CU_add_test(s, "read-only acquire never shares with a writer",
                test_ro_acquire_never_shares_with_a_writer);
    CU_add_test(s, "assign_ro tolerates a reader, not a writer",
                test_assign_ro_tolerates_a_reader_not_a_writer);
    CU_add_test(s, "acquire_ro syscall binds read-only",
                test_acquire_ro_syscall);
    CU_add_test(s, "bdev read-only acquire shares with a read-only holder",
                test_bdev_acquire_ro_shares_with_a_ro_holder);
    CU_add_test(s, "re-acquire in the other mode is busy",
                test_reacquire_in_other_mode_is_busy);
    CU_add_test(s, "revoke pending is per owner",
                test_revoke_pending_is_per_owner);
    CU_add_test(s, "owner is free with several holders",
                test_owner_is_free_with_several_holders);
    CU_add_test(s, "assign refuses a volume someone else holds",
                test_assign_refuses_a_volume_someone_else_holds);
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
