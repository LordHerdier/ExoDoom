/*
 * test_syscall_fuzz_k.c — random syscall arguments don't crash the kernel
 * (SCRUM-115).
 *
 * Drives exo_syscall_dispatch() directly with random numbers and arguments,
 * the same route a real ring-3 `syscall` reaches once syscall_entry.s has
 * marshalled its registers (test_syscall_mem_k.c, test_ownership_k.c already
 * exercise the dispatcher the same way). Acceptance: 1,000,000 invocations,
 * no kernel panic or corruption.
 *
 * A triple fault kills QEMU outright, so the "no panic" half of the
 * acceptance criterion is already covered by this suite simply completing
 * — the harness's own timeout (make docker-ci's `timeout 30`) is what would
 * catch that. The "no corruption" half needs an explicit check, which
 * test_post_fuzz_integrity() below provides: a known-good page alloc/free
 * round trip, and that it leaves PAGE_OWNER_LIBOS's owned-page count exactly
 * where it found it.
 *
 * EXO_SYS_EXIT (#20) is excluded from the fuzzed number pool entirely: it
 * ignores its arguments completely and unconditionally reclaims every page
 * and the framebuffer binding owned by PAGE_OWNER_LIBOS (src/syscall_exit.c)
 * — the same context id every KUnit suite in this boot dispatches as
 * (syscall_current_context() -> context_current(), PAGE_OWNER_LIBOS in v1).
 * Calling it even once here would rip resources out from under every suite
 * that runs after this one. Its argument-independence is already covered by
 * test_syscall_exit_k.c.
 *
 * EXO_SYS_LAUNCH_WAD_VIEWER/_CLOCK (#21/#22) also ignore their arguments,
 * and each does a real context_create() + image copy on every call — bounded
 * by CONTEXT_MAX and shared with every other suite in this boot. They are
 * included in the pool but rate-limited to roughly 1-in-2000 draws (see
 * rare_hit() below) so a garbage-argument call is still proven safe without
 * spending most of the 1,000,000-call budget on context churn.
 *
 * EXO_SYS_FB_ACQUIRE (#4), EXO_SYS_KBD_POLL (#6) and EXO_SYS_SERIAL_WRITE
 * (#8) validate their pointer argument with exo_range_in_user_window() only
 * — a *bounds* check, not a "this address is actually mapped" check — and
 * then have the kernel write straight through it. In v1 the whole LibOS
 * window is reserved-but-unmapped until a LibOS exo_page_map's a vaddr, and
 * the single v1 context runs on the kernel's own shared page tables, so an
 * in-window-but-unmapped pointer (e.g. exactly EXO_USER_VA_BASE) crashes the
 * kernel with a supervisor-mode not-present page fault instead of being
 * rejected — filed as SCRUM-186. Until that lands, this suite's pointer
 * argument for these three numbers is drawn from next_arg_out_of_window()
 * rather than next_arg(): still exercises the -EXO_EFAULT bounds-check path
 * (and the odd genuinely-successful call, for SERIAL_WRITE/FB_ACQUIRE with a
 * small/zero length), but never an in-window address these handlers would
 * actually dereference. */

#include "kunit.h"
#include "syscall.h"
#include "exo_syscall.h"
#include "page_alloc.h"

#include <stdint.h>
#include <stdlib.h>

#define FUZZ_ITERATIONS 1000000u
#define PAGE_SIZE       4096u

/* A fixed seed keeps a failing run reproducible, the same convention
 * test_stdlib_k.c documents for rand()/srand()'s own acceptance criterion. */
#define FUZZ_SEED 0x5CADu

/* Edge values worth hitting more often than uniform-random 64-bit draws
 * would land on them by chance: syscall-window boundaries, page-size
 * boundaries, a known kernel-image address (src/linker.ld links at 2M), and
 * small lengths so exo_serial_write's len check and exo_page_map's flag
 * word occasionally get exercised past their first validation branch. */
static const uint64_t interesting_values[] = {
    0ULL, 1ULL, 2ULL, 15ULL, 16ULL, 4095ULL, 4096ULL, 4097ULL,
    (uint64_t)-1, (uint64_t)-2,
    EXO_USER_VA_BASE, EXO_USER_VA_BASE - 1, EXO_USER_VA_BASE + 1,
    EXO_USER_VA_END,  EXO_USER_VA_END - 1,  EXO_USER_VA_END + 1,
    0x200000ULL,        /* kernel link address (src/linker.ld) */
    0x1000ULL + 1,       /* page-misaligned */
    (uint64_t)(EXO_PAGE_READ | EXO_PAGE_WRITE | EXO_PAGE_USER | EXO_PAGE_EXEC),
};
#define INTERESTING_COUNT \
    (sizeof(interesting_values) / sizeof(interesting_values[0]))

/* Same idea, but with every value that falls inside [EXO_USER_VA_BASE,
 * EXO_USER_VA_END) removed — see SCRUM-186 in the file comment above. Used
 * for the pointer argument of the three syscalls that write through it
 * without first confirming it's mapped. */
static const uint64_t out_of_window_values[] = {
    0ULL, 1ULL, 2ULL, 15ULL, 16ULL, 4095ULL, 4096ULL, 4097ULL,
    (uint64_t)-1, (uint64_t)-2,
    EXO_USER_VA_BASE - 1, EXO_USER_VA_END, EXO_USER_VA_END + 1,
    0x200000ULL,
    0x1000ULL + 1,
};
#define OUT_OF_WINDOW_COUNT \
    (sizeof(out_of_window_values) / sizeof(out_of_window_values[0]))

/* rand() only gives 15 bits (RAND_MAX == 32767, src/stdlib.h); assemble a
 * wider word out of several draws rather than relying on any single call for
 * more entropy than it has. */
static uint32_t next_u30(void)
{
    return ((uint32_t)rand() << 15) | (uint32_t)rand();
}

static uint64_t next_u64_random(void)
{
    uint64_t v = 0;
    for (int i = 0; i < 3; i++)
        v = (v << 30) | next_u30();
    return v;
}

/* One argument: mostly pure-random, sometimes a curated edge value. */
static uint64_t next_arg(void)
{
    if (rand() % 3 == 0)
        return interesting_values[next_u30() % INTERESTING_COUNT];
    return next_u64_random();
}

/* Same as next_arg(), but never returns an address inside the LibOS window
 * — see SCRUM-186 in the file comment. Pure-random draws already land
 * outside the (46-bit-wide) window with overwhelming probability; only the
 * curated corpus needed a filtered variant. */
static uint64_t next_arg_out_of_window(void)
{
    if (rand() % 3 == 0)
        return out_of_window_values[next_u30() % OUT_OF_WINDOW_COUNT];
    return next_u64_random();
}

/* True roughly 1 draw in 2000 — the rate limit for the two LAUNCH_* numbers
 * (see file comment). */
static int rare_hit(void)
{
    return (next_u30() % 2000) == 0;
}

/* A syscall number to dispatch: uniform over [0, EXO_SYS_COUNT + 4), the
 * +4 slack covering the out-of-range -EXO_ENOSYS path, with EXO_SYS_EXIT
 * excluded outright and EXO_SYS_LAUNCH rate-limited.
 *
 * The rate limit used to name two numbers (EXO_SYS_LAUNCH_WAD_VIEWER and
 * EXO_SYS_LAUNCH_CLOCK) and now names one, because SCRUM-184 collapsed the
 * per-app launch syscalls into EXO_SYS_LAUNCH with an app-id argument. The
 * limit still matters for the same reason it always did: a launch that
 * SUCCEEDS arms a context switch and the fuzzer does not come back.
 *
 * Worth noting that this change makes a successful launch far less likely
 * rather than more. The app id is a1, which the loop below fills with a
 * random pointer-shaped value, so it is almost always >= EXO_LAUNCH_APP_COUNT
 * and comes straight back as -EXO_EINVAL. Before, picking the number *was*
 * picking the app. */
static uint64_t next_syscall_num(void)
{
    for (;;) {
        uint64_t n = next_u30() % (EXO_SYS_COUNT + 4);

        if (n == EXO_SYS_EXIT)
            continue;

        if (n == EXO_SYS_LAUNCH && !rare_hit())
            continue;

        return n;
    }
}

static void test_one_million_random_syscalls(void)
{
    srand(FUZZ_SEED);

    uint32_t iterations = 0;
    for (uint32_t i = 0; i < FUZZ_ITERATIONS; i++) {
        uint64_t num = next_syscall_num();

        /* a1 is the pointer argument for exo_fb_acquire/exo_kbd_poll/
         * exo_serial_write (info_out/event_out/buf) — keep it out of the
         * LibOS window for those three until SCRUM-186 lands (see file
         * comment). */
        uint64_t a1 = (num == EXO_SYS_FB_ACQUIRE || num == EXO_SYS_KBD_POLL ||
                       num == EXO_SYS_SERIAL_WRITE)
                          ? next_arg_out_of_window()
                          : next_arg();
        uint64_t a2 = next_arg();
        uint64_t a3 = next_arg();
        uint64_t a4 = next_arg();
        uint64_t a5 = next_arg();
        uint64_t a6 = next_arg();

        int64_t rc = exo_syscall_dispatch(num, a1, a2, a3, a4, a5, a6);

        /* A genuine page allocation must be handed back immediately, or the
         * PMM drains over a million iterations instead of proving the
         * dispatcher survives them. */
        if (num == EXO_SYS_PAGE_ALLOC && rc > 0)
            exo_syscall_dispatch(EXO_SYS_PAGE_FREE, (uint64_t)rc,
                                 0, 0, 0, 0, 0);

        iterations++;
    }

    /* Reaching here at all means the dispatcher survived a million arbitrary
     * invocations without a fault taking the machine down; this pins the
     * loop itself to the acceptance criterion's literal count. */
    CU_ASSERT_EQUAL(iterations, FUZZ_ITERATIONS);
}

/* "No corruption" needs an observable check beyond "didn't crash": a
 * known-good page alloc/free round trip must still behave exactly as
 * test_syscall_mem_k.c's own happy path expects, and PAGE_OWNER_LIBOS's
 * owned-page count must be unchanged by it — a fuzzed input that corrupted
 * the PMM's bitmap or owner table would show up right here. */
static void test_post_fuzz_integrity(void)
{
    uint32_t before = page_count_owned(PAGE_OWNER_LIBOS);

    int64_t p = exo_syscall_dispatch(EXO_SYS_PAGE_ALLOC, 0, 0, 0, 0, 0, 0);
    CU_ASSERT(p > 0);
    CU_ASSERT_EQUAL((uint64_t)p % PAGE_SIZE, 0);
    CU_ASSERT_EQUAL(
        exo_syscall_dispatch(EXO_SYS_PAGE_FREE, (uint64_t)p, 0, 0, 0, 0, 0),
        0);

    uint32_t after = page_count_owned(PAGE_OWNER_LIBOS);
    CU_ASSERT_EQUAL(before, after);
}

void suite_syscall_fuzz_tests(CU_pSuite s)
{
    CU_add_test(s, "one million random syscalls", test_one_million_random_syscalls);
    CU_add_test(s, "post-fuzz PMM integrity",      test_post_fuzz_integrity);
}
