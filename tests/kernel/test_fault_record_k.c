/*
 * test_fault_record_k.c — the fault log and exo_fault_info (#41) (SCRUM-283).
 *
 * The ring's contract is seq arithmetic: numbers from 1 that never repeat,
 * "newer than N" reads, and overwrite-oldest once EXO_FAULT_LOG_MAX are held.
 * Pure logic, so it is checked without faulting anything; the live path that
 * fills the log is test_fault_contain_k.c.
 */

#include "kunit.h"
#include "fault_record.h"
#include "exo_syscall.h"
#include "syscall.h"

#include <stdint.h>

int fault_record_suite_cleanup(void)
{
    fault_record_reset();
    return 0;
}

static void add(uint64_t id)
{
    fault_record_add(id, "prog", 14, EXO_FAULT_REASON_MEMORY, 0x1000 + id, 0);
}

static void test_empty_log_has_nothing(void)
{
    fault_record_reset();
    exo_fault_info_t r;
    CU_ASSERT_EQUAL(fault_record_last_seq(), 0);
    CU_ASSERT_EQUAL(fault_record_next(0, &r), -1);
}

static void test_reads_walk_forward_in_order(void)
{
    fault_record_reset();
    add(10); add(11); add(12);

    exo_fault_info_t r;
    uint64_t seen = 0;
    for (uint64_t expect = 1; expect <= 3; expect++) {
        CU_ASSERT_EQUAL(fault_record_next(seen, &r), 0);
        CU_ASSERT_EQUAL(r.seq, expect);
        CU_ASSERT_EQUAL(r.ctx_id, 9 + expect);
        seen = r.seq;
    }
    CU_ASSERT_EQUAL(fault_record_next(seen, &r), -1);
    CU_ASSERT_EQUAL(fault_record_next(99, &r), -1);
}

static void test_full_ring_drops_the_oldest(void)
{
    fault_record_reset();
    for (uint64_t i = 1; i <= EXO_FAULT_LOG_MAX + 3; i++)
        add(i);

    /* Asking from the start lands on the oldest record still held, not on a
     * stale slot that was overwritten. */
    exo_fault_info_t r;
    CU_ASSERT_EQUAL(fault_record_next(0, &r), 0);
    CU_ASSERT_EQUAL(r.seq, 4);
    CU_ASSERT_EQUAL(r.ctx_id, 4);

    CU_ASSERT_EQUAL(fault_record_next(EXO_FAULT_LOG_MAX + 2, &r), 0);
    CU_ASSERT_EQUAL(r.seq, EXO_FAULT_LOG_MAX + 3);
    CU_ASSERT_EQUAL(fault_record_last_seq(), EXO_FAULT_LOG_MAX + 3);
}

/* EXO_FAULT_NEWEST: what a restarted shell asks for, so it reports the crash
 * that explains it and nothing older. */
static void test_newest_returns_only_the_latest(void)
{
    exo_fault_info_t r;

    fault_record_reset();
    CU_ASSERT_EQUAL(fault_record_next(EXO_FAULT_NEWEST, &r), -1);

    add(1); add(2); add(3);
    CU_ASSERT_EQUAL(fault_record_next(EXO_FAULT_NEWEST, &r), 0);
    CU_ASSERT_EQUAL(r.seq, 3);
    CU_ASSERT_EQUAL(r.ctx_id, 3);

    /* Still the newest after the ring has wrapped. */
    for (uint64_t i = 4; i <= EXO_FAULT_LOG_MAX + 5; i++)
        add(i);
    CU_ASSERT_EQUAL(fault_record_next(EXO_FAULT_NEWEST, &r), 0);
    CU_ASSERT_EQUAL(r.seq, EXO_FAULT_LOG_MAX + 5);
    CU_ASSERT_EQUAL(r.ctx_id, EXO_FAULT_LOG_MAX + 5);

    /* Reading "newer than the newest" afterwards finds nothing, so a shell
     * that started from NEWEST is not told the same crash twice. */
    CU_ASSERT_EQUAL(fault_record_next(r.seq, &r), -1);
}

static void test_name_is_copied_and_terminated(void)
{
    fault_record_reset();
    fault_record_add(1, "an-overlong-program-name", 6, EXO_FAULT_REASON_ILLEGAL, 0, 0);
    fault_record_add(2, 0, 0, EXO_FAULT_REASON_ARITHMETIC, 0, 0);

    exo_fault_info_t r;
    CU_ASSERT_EQUAL(fault_record_next(0, &r), 0);
    CU_ASSERT_EQUAL(r.name[sizeof r.name - 1], '\0');
    CU_ASSERT_EQUAL(r.name[0], 'a');
    CU_ASSERT_EQUAL(fault_record_next(1, &r), 0);
    CU_ASSERT_EQUAL(r.name[0], '\0');
}

/* From ring 0 there is no mapped user page to write to, so the success path
 * is the ring's; what the handler adds is the pointer check, which comes
 * before anything is looked up. */
static void test_syscall_rejects_a_bad_pointer(void)
{
    fault_record_reset();
    add(1);
    CU_ASSERT_EQUAL(exo_syscall_dispatch(EXO_SYS_FAULT_INFO, 0, 0, 0, 0, 0, 0),
                    -EXO_EFAULT);
    CU_ASSERT_EQUAL(exo_syscall_dispatch(EXO_SYS_FAULT_INFO, 0, 0x1000, 0, 0, 0, 0),
                    -EXO_EFAULT);
}

void suite_fault_record_tests(CU_pSuite s)
{
    CU_add_test(s, "empty log has nothing to read", test_empty_log_has_nothing);
    CU_add_test(s, "reads walk forward in seq order", test_reads_walk_forward_in_order);
    CU_add_test(s, "a full ring drops the oldest record", test_full_ring_drops_the_oldest);
    CU_add_test(s, "EXO_FAULT_NEWEST returns only the latest record", test_newest_returns_only_the_latest);
    CU_add_test(s, "name is truncated and NUL-terminated", test_name_is_copied_and_terminated);
    CU_add_test(s, "exo_fault_info rejects a bad pointer", test_syscall_rejects_a_bad_pointer);
}
