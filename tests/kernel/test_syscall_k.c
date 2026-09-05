/*
 * test_syscall_k.c — syscall entry path (SCRUM-32).
 *
 * Three layers, cheapest first, so a failure localises itself:
 *
 *   1. the MSRs are programmed the way syscall/sysret require;
 *   2. the C dispatcher range-checks and routes;
 *   3. a real `syscall` from ring 3 makes the whole round trip with every
 *      preserved register intact.
 *
 * (3) is the acceptance criterion, and it is the reason this file exists
 * rather than a comment in the header asserting the same thing.  The harness
 * it drives lives in tests/kernel/ring3_probe.s.
 */

#include "kunit.h"
#include "syscall.h"
#include "exo_syscall.h"
#include "msr.h"
#include "serial.h"

/* ── Ring-3 harness (tests/kernel/ring3_probe.s) ─────────────────────────── */

extern uint64_t ring3_run(void (*entry)(void), void *user_stack_top);
extern void     ring3_probe(void);
extern int64_t  ring3_escape(uint64_t result, uint64_t, uint64_t,
                             uint64_t, uint64_t, uint64_t);

/* These four must match the .set values in ring3_probe.s. */
#define SYS_ECHO    19          /* borrowed EXO_SYS_YIELD */
#define SYS_ESCAPE  20          /* borrowed EXO_SYS_EXIT  */
#define ECHO_RET    0x5EC0DE

/* Bit assignments in the probe's failure mask, for a legible diagnosis. */
static const char *const probe_bit_name[] = {
    "rdi", "rsi", "rdx", "r10", "r8", "r9",
    "rbx", "rbp", "r12", "r13", "r14", "r15",
    "return value",
    "probe was not at CPL 3",
};

/* 16 KiB of ring-3 stack.  Writable from CPL 3 only because the TESTING
 * build sets U/S through the identity map — see src/boot.s. */
static uint8_t user_stack[16384] __attribute__((aligned(16)));

/* What the echo handler last saw, so the ring-3 path can be checked for
 * argument marshalling too and not just preservation. */
static volatile uint64_t echo_args[6];
static volatile int      echo_calls;

static int64_t echo_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    echo_args[0] = a1;
    echo_args[1] = a2;
    echo_args[2] = a3;
    echo_args[3] = a4;
    echo_args[4] = a5;
    echo_args[5] = a6;
    echo_calls++;

    return ECHO_RET;
}

/* ── 1. MSR programming ──────────────────────────────────────────────────── */

static void test_efer_enables_syscall(void)
{
    uint64_t efer = rdmsr(MSR_IA32_EFER);

    /* SCE is what makes the `syscall` instruction legal at all. */
    CU_ASSERT((efer & EFER_SCE) != 0);

    /* And syscall_init must not have clobbered what boot.s put here: losing
     * LME/LMA would take the CPU out of long mode. */
    CU_ASSERT((efer & EFER_LME) != 0);
    CU_ASSERT((efer & EFER_LMA) != 0);
}

static void test_star_selectors(void)
{
    uint64_t star = rdmsr(MSR_IA32_STAR);

    /* syscall: CS = STAR[47:32], SS = that + 8. */
    CU_ASSERT_EQUAL((star >> 32) & 0xFFFF, 0x08);

    /* sysret: CS = STAR[63:48] + 16, SS = STAR[63:48] + 8.  0x18|3 is what
     * puts those at the user code64 (0x28) and user data (0x20) descriptors
     * boot.s defines — get this wrong and every return faults. */
    CU_ASSERT_EQUAL((star >> 48) & 0xFFFF, 0x18 | 3);
}

static void test_lstar_points_at_entry(void)
{
    CU_ASSERT_EQUAL(rdmsr(MSR_IA32_LSTAR), (uint64_t)(uintptr_t)&syscall_entry);
}

static void test_fmask_clears_interrupt_flag(void)
{
    uint64_t fmask = rdmsr(MSR_IA32_FMASK);

    /* IF above all: the entry stub parks the user RSP in a single global and
     * is not reentrant, so an interrupt arriving mid-entry would be fatal. */
    CU_ASSERT((fmask & 0x200) != 0);

    /* DF, because the SysV ABI lets compiled code assume it is clear. */
    CU_ASSERT((fmask & 0x400) != 0);
}

/* ── 2. Dispatcher ───────────────────────────────────────────────────────── */

static void test_dispatch_rejects_out_of_range(void)
{
    CU_ASSERT_EQUAL(exo_syscall_dispatch(EXO_SYS_COUNT, 0, 0, 0, 0, 0, 0),
                    -EXO_ENOSYS);

    /* A "negative" number from ring 3 is just a very large unsigned one; the
     * unsigned compare in the dispatcher has to catch it as the same case,
     * because a signed compare would index the table backwards. */
    CU_ASSERT_EQUAL(exo_syscall_dispatch((uint64_t)-1, 0, 0, 0, 0, 0, 0),
                    -EXO_ENOSYS);
}

static void test_dispatch_rejects_unbound_number(void)
{
    /* In range, nothing registered: SCRUM-32 ships no handlers. */
    CU_ASSERT_PTR_NULL(exo_syscall_handler(EXO_SYS_GET_TICKS));
    CU_ASSERT_EQUAL(exo_syscall_dispatch(EXO_SYS_GET_TICKS, 0, 0, 0, 0, 0, 0),
                    -EXO_ENOSYS);
}

static void test_dispatch_routes_all_six_arguments(void)
{
    exo_syscall_register(SYS_ECHO, echo_handler);
    echo_calls = 0;

    int64_t ret = exo_syscall_dispatch(SYS_ECHO, 1, 2, 3, 4, 5, 6);

    CU_ASSERT_EQUAL(ret, ECHO_RET);
    CU_ASSERT_EQUAL(echo_calls, 1);
    CU_ASSERT_EQUAL(echo_args[0], 1);
    CU_ASSERT_EQUAL(echo_args[1], 2);
    CU_ASSERT_EQUAL(echo_args[2], 3);
    CU_ASSERT_EQUAL(echo_args[3], 4);
    CU_ASSERT_EQUAL(echo_args[4], 5);
    CU_ASSERT_EQUAL(echo_args[5], 6);

    exo_syscall_register(SYS_ECHO, 0);
}

static void test_register_unbinds_with_null(void)
{
    exo_syscall_register(SYS_ECHO, echo_handler);
    CU_ASSERT_PTR_NOT_NULL(exo_syscall_handler(SYS_ECHO));

    exo_syscall_register(SYS_ECHO, 0);
    CU_ASSERT_PTR_NULL(exo_syscall_handler(SYS_ECHO));
    CU_ASSERT_EQUAL(exo_syscall_dispatch(SYS_ECHO, 0, 0, 0, 0, 0, 0),
                    -EXO_ENOSYS);

    /* Out-of-range registration is ignored rather than scribbling past the
     * table — the number reaching here is caller-supplied. */
    exo_syscall_register(EXO_SYS_COUNT, echo_handler);
    CU_ASSERT_PTR_NULL(exo_syscall_handler(EXO_SYS_COUNT));
}

/* ── 3. Ring-3 round trip ────────────────────────────────────────────────── */

static void test_ring3_syscall_preserves_registers(void)
{
    exo_syscall_register(SYS_ECHO, echo_handler);
    exo_syscall_register(SYS_ESCAPE, ring3_escape);
    echo_calls = 0;

    uint64_t failures = ring3_run(ring3_probe,
                                  user_stack + sizeof(user_stack));

    /* Both echo calls landed — the second is the one that catches a stub
     * which preserves only the callee-saved set. */
    CU_ASSERT_EQUAL(echo_calls, 2);

    /* Name whatever broke before the bare assertion, so a CI log says which
     * register rather than just "0 != 64". */
    for (unsigned b = 0; b < sizeof(probe_bit_name) / sizeof(*probe_bit_name);
         b++) {
        if (failures & (1ull << b)) {
            serial_print("    ring-3 probe: not preserved: ");
            serial_print(probe_bit_name[b]);
            serial_print("\n");
        }
    }

    CU_ASSERT_EQUAL(failures, 0);

    exo_syscall_register(SYS_ECHO, 0);
    exo_syscall_register(SYS_ESCAPE, 0);
}

static void test_ring3_syscall_delivers_arguments(void)
{
    /* Runs its own excursion rather than reading what the previous test left
     * behind — an assertion that silently depends on suite ordering fails in
     * a baffling way the first time someone reorders the registrations. */
    exo_syscall_register(SYS_ECHO, echo_handler);
    exo_syscall_register(SYS_ESCAPE, ring3_escape);

    (void)ring3_run(ring3_probe, user_stack + sizeof(user_stack));

    exo_syscall_register(SYS_ECHO, 0);
    exo_syscall_register(SYS_ESCAPE, 0);

    /* The probe holds its sentinels in the argument registers across the
     * syscall, so what the handler received is a check that the entry stub's
     * RAX/RDI/RSI/RDX/R10/R8/R9 -> SysV shuffle lands each one in the right
     * parameter — a rotation that is easy to get subtly wrong. */
    CU_ASSERT_EQUAL(echo_args[0], 0xA1A1A1A1A1A1A1A1ull);
    CU_ASSERT_EQUAL(echo_args[1], 0xB2B2B2B2B2B2B2B2ull);
    CU_ASSERT_EQUAL(echo_args[2], 0xC3C3C3C3C3C3C3C3ull);
    CU_ASSERT_EQUAL(echo_args[3], 0xD4D4D4D4D4D4D4D4ull);
    CU_ASSERT_EQUAL(echo_args[4], 0xE5E5E5E5E5E5E5E5ull);
    CU_ASSERT_EQUAL(echo_args[5], 0xF6F6F6F6F6F6F6F6ull);
}

void suite_syscall_tests(CU_pSuite s)
{
    CU_add_test(s, "efer enables syscall",        test_efer_enables_syscall);
    CU_add_test(s, "star selectors",              test_star_selectors);
    CU_add_test(s, "lstar points at entry",       test_lstar_points_at_entry);
    CU_add_test(s, "fmask clears IF",             test_fmask_clears_interrupt_flag);

    CU_add_test(s, "dispatch rejects out of range", test_dispatch_rejects_out_of_range);
    CU_add_test(s, "dispatch rejects unbound",      test_dispatch_rejects_unbound_number);
    CU_add_test(s, "dispatch routes six args",      test_dispatch_routes_all_six_arguments);
    CU_add_test(s, "register unbinds with NULL",    test_register_unbinds_with_null);

    CU_add_test(s, "ring3 preserves registers",   test_ring3_syscall_preserves_registers);
    CU_add_test(s, "ring3 delivers arguments",    test_ring3_syscall_delivers_arguments);
}
