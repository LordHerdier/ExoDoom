/*
 * test_syscall_bench_k.c — syscall round-trip benchmarks (SCRUM-60).
 *
 * Blocked on SCRUM-33 (exo_get_ticks, the first end-to-end syscall); that
 * landed long ago, and six more syscalls have since been bound, so this
 * suite covers exo_page_alloc/_free (#0/#1), exo_page_map/_unmap (#2/#3),
 * exo_fb_acquire (#4), exo_get_ticks (#5), exo_kbd_poll (#6) and
 * exo_serial_write (#8) -- plus the dispatcher's bare -EXO_ENOSYS floor via
 * the still-unbound exo_mouse_poll (#7), a useful "no handler work at all"
 * baseline to set the others against.
 *
 * Method: tests/kernel/syscall_bench_probe.s's three entry points, launched
 * through the real libos_build_image()/libos_enter() mechanism (SCRUM-49/
 * -50) -- NOT tests/kernel/ring3_probe.s's ring3_run()/ring3_escape(),
 * which src/vmm.c (SCRUM-55) restricts to exactly its own two legacy
 * probes' byte ranges. Each probe reads its syscall number/args/iteration
 * count from a small params struct this file builds and hands to
 * libos_build_image() as the launched image's .data, loops that many
 * syscalls back to back with interrupts masked (so nothing preempts the
 * timing), and returns the total elapsed `rdtsc` delta through
 * libos_return() the same way every other launch probe in this tree
 * escapes.
 *
 * Two owner ids are in play, for the reason libos_test_common.h documents
 * on TEST_OWNER_SYSCALL_BENCH's neighbors: exo_page_map/_unmap resolve
 * "which address space is this" via syscall_current_context(), hardcoded
 * in v1 to PAGE_OWNER_LIBOS regardless of which owner a test launches
 * under -- so the map/unmap benchmark launches under PAGE_OWNER_LIBOS
 * itself (like test_libc_shim_probe_k.c), saving and restoring its
 * boot-time binding around the test. Every other benchmark here either
 * touches no address-space state (get_ticks, kbd_poll, serial_write, the
 * ENOSYS floor) or only tags pages by the same hardcoded context
 * regardless of the launching owner (page_alloc/free), so those all launch
 * under the ordinary TEST_OWNER_SYSCALL_BENCH sentinel.
 *
 * Assertions here are sanity bounds, not a regression gate: QEMU's TCG
 * `rdtsc` reflects the host machine's real clock, which varies across
 * developer machines and CI runners. The numbers themselves -- printed to
 * serial by every case below -- are the deliverable; see
 * docs/syscall_spec.md's benchmark subsection for the baseline this suite
 * produced.
 */

#include "kunit.h"
#include "syscall.h"
#include "exo_syscall.h"
#include "libos_launch.h"
#include "libos_test_common.h"
#include "vmm.h"
#include "ps2.h"
#include "serial.h"

#include <stdint.h>
#include <stddef.h>

extern void bench_probe_simple(void);
extern void bench_probe_simple_end(void);
extern void bench_probe_page_alloc_free(void);
extern void bench_probe_page_alloc_free_end(void);
extern void bench_probe_page_map_unmap(void);
extern void bench_probe_page_map_unmap_end(void);

/* Must match syscall_bench_probe.s's bench_probe_simple offset comment. */
typedef struct {
    uint64_t sysnum;
    uint64_t a1, a2, a3, a4;
    uint64_t iterations;
} bench_simple_params_t;

/* Must match bench_probe_page_alloc_free's offset comment. */
typedef struct {
    uint64_t iterations;
} bench_pair_params_t;

/* Must match bench_probe_page_map_unmap's offset comment. */
typedef struct {
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t flags;
    uint64_t iterations;
} bench_map_params_t;

/* Scratch buffer inside the launched image's own data page, past the
 * params struct -- big enough for exo_fb_info_t (the largest out-param any
 * case here writes) with headroom. Never read by this file; exo_fb_acquire
 * writes it, exo_kbd_poll and exo_serial_write(len=0) never touch it. */
#define BENCH_SCRATCH_OFFSET 64
#define BENCH_DATA_LEN       128

#define SIMPLE_ITERATIONS 5000
#define PAIR_ITERATIONS   1000

/* Runs `code_len` bytes of `code` under TEST_OWNER_SYSCALL_BENCH with
 * `params`/`params_len` as its .data, launches it, tears it down, and
 * returns the cycle count libos_return() got -- or 0 with `*ok` cleared if
 * anything along the way didn't come back clean. */
static uint64_t run_bench(const void *code, size_t code_len,
                          const void *params, size_t params_len, int *ok)
{
    libos_image_t img;
    *ok = 0;

    if (libos_build_image(TEST_OWNER_SYSCALL_BENCH, code, code_len,
                          params, params_len, BENCH_DATA_LEN - params_len,
                          &img) != VMM_OK) {
        return 0;
    }

    libos_test_launch_result_t run = libos_test_launch(&img);
    libos_destroy_image(TEST_OWNER_SYSCALL_BENCH, &img);

    if (run.switch_in_status != VMM_OK || run.switch_out_status != VMM_OK ||
       run.fault_count != 0) {
        return 0;
    }

    *ok = 1;
    return run.result;
}

static void report(const char *name, uint64_t total_cycles,
                   uint64_t syscalls)
{
    uint32_t per_call = (uint32_t)(total_cycles / syscalls);

    serial_print("  syscall_bench: ");
    serial_print(name);
    serial_print(": ");
    serial_print_dec(per_call);
    serial_print(" cycles/syscall (");
    serial_print_dec((uint32_t)syscalls);
    serial_print(" calls)\n");

    /* Sanity bounds only -- see file header. Anti-hang/anti-fault guard,
     * not a performance gate: a genuine 10M-cycle syscall would mean
     * something is badly broken (an accidental busy-loop, a missed
     * dispatch), not just "slow under TCG". */
    CU_ASSERT(per_call > 0);
    CU_ASSERT(per_call < 10000000u);
}

static void bench_simple_case(const char *name, uint64_t sysnum,
                              uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4)
{
    bench_simple_params_t params = {
        .sysnum = sysnum, .a1 = a1, .a2 = a2, .a3 = a3, .a4 = a4,
        .iterations = SIMPLE_ITERATIONS,
    };

    size_t code_len = (uintptr_t)&bench_probe_simple_end -
                      (uintptr_t)&bench_probe_simple;

    int ok;
    uint64_t cycles = run_bench(&bench_probe_simple, code_len,
                               &params, sizeof(params), &ok);
    CU_ASSERT(ok);
    if (!ok) {
        return;
    }

    report(name, cycles, SIMPLE_ITERATIONS);
}

static void test_bench_get_ticks(void)
{
    bench_simple_case("exo_get_ticks        (#5)", EXO_SYS_GET_TICKS,
                      0, 0, 0, 0);
}

static void test_bench_kbd_poll(void)
{
    /* Empty ring for the whole run: sys_kbd_poll never dereferences
     * event_out when it has nothing to dequeue (src/syscall_kbd.c), so the
     * scratch address below is never actually touched. kbd_ring is a
     * single kernel-global instance shared with every other suite -- reset
     * it first so a leftover event from an earlier suite can't turn one
     * call into the (slower, dereferencing) non-empty path. */
    kbd_reset();
    bench_simple_case("exo_kbd_poll (empty) (#6)", EXO_SYS_KBD_POLL,
                      LIBOS_LAUNCH_DATA_VADDR + BENCH_SCRATCH_OFFSET,
                      0, 0, 0);
}

static void test_bench_serial_write(void)
{
    /* len=0 always succeeds without touching COM1 or buf
     * (docs/syscall_spec.md §3.2 #8) -- this measures dispatch overhead,
     * not UART bandwidth. */
    bench_simple_case("exo_serial_write(0)  (#8)", EXO_SYS_SERIAL_WRITE,
                      LIBOS_LAUNCH_DATA_VADDR + BENCH_SCRATCH_OFFSET,
                      0, 0, 0);
}

static void test_bench_fb_acquire(void)
{
    /* A re-acquire by the current owner is a defined, idempotent success
     * (docs/syscall_spec.md §3.2 #4) that re-fills info_out every time --
     * the scratch offset is inside the launched image's own mapped data
     * page, so the write always lands somewhere real. */
    bench_simple_case("exo_fb_acquire       (#4)", EXO_SYS_FB_ACQUIRE,
                      LIBOS_LAUNCH_DATA_VADDR + BENCH_SCRATCH_OFFSET,
                      0, 0, 0);
}

static void test_bench_dispatcher_floor(void)
{
    /* exo_mouse_poll (#7) has no handler bound (docs/syscall_spec.md §3.2)
     * -- the dispatcher's range check runs and it returns -EXO_ENOSYS
     * immediately, with no handler body at all. This is the floor every
     * other number's "handler work" cost sits on top of. */
    bench_simple_case("dispatcher floor (ENOSYS, #7)", EXO_SYS_MOUSE_POLL,
                      0, 0, 0, 0);
}

static void test_bench_page_alloc_free(void)
{
    bench_pair_params_t params = { .iterations = PAIR_ITERATIONS };

    size_t code_len = (uintptr_t)&bench_probe_page_alloc_free_end -
                      (uintptr_t)&bench_probe_page_alloc_free;

    int ok;
    uint64_t cycles = run_bench(&bench_probe_page_alloc_free, code_len,
                               &params, sizeof(params), &ok);
    CU_ASSERT(ok);
    if (!ok) {
        return;
    }

    report("exo_page_alloc+free  (#0/#1)", cycles, 2 * PAIR_ITERATIONS);
}

/* Scratch vaddr for the map/unmap benchmark's target page, distinct from
 * every other suite's own EXO_USER_VA_BASE offset (see test_syscall_kbd_k.c,
 * test_syscall_serial_k.c, test_fb_binding_k.c, test_page_map_k.c,
 * test_vmm_k.c, test_revoke_k.c for the ones already claimed). */
#define MAP_BENCH_VADDR (EXO_USER_VA_BASE + 0x60000000ULL)

/*
 * PAGE_OWNER_LIBOS, not TEST_OWNER_SYSCALL_BENCH -- see this file's header
 * and libos_test_common.h's comment on TEST_OWNER_SYSCALL_BENCH's
 * neighbors. exo_page_map/exo_page_unmap resolve the caller's address
 * space through syscall_current_context(), hardcoded to PAGE_OWNER_LIBOS;
 * launching under any other id would map into whatever address space
 * PAGE_OWNER_LIBOS happens to be bound to, not the one actually loaded in
 * CR3 for this probe.
 */
static uint64_t saved_libos_pml4;

int syscall_bench_suite_init(void)
{
    saved_libos_pml4 = vmm_address_space_for(PAGE_OWNER_LIBOS);
    return 0;
}

static void test_bench_page_map_unmap(void)
{
    int64_t alloc_rc = exo_syscall_dispatch(EXO_SYS_PAGE_ALLOC, 0, 0, 0,
                                            0, 0, 0);
    CU_ASSERT(alloc_rc >= 0);
    if (alloc_rc < 0) {
        return;
    }
    uint64_t paddr = (uint64_t)alloc_rc;

    bench_map_params_t params = {
        .vaddr = MAP_BENCH_VADDR,
        .paddr = paddr,
        .flags = EXO_PAGE_WRITE | EXO_PAGE_USER,
        .iterations = PAIR_ITERATIONS,
    };

    size_t code_len = (uintptr_t)&bench_probe_page_map_unmap_end -
                      (uintptr_t)&bench_probe_page_map_unmap;

    libos_image_t img;
    int build_ok = (libos_build_image(PAGE_OWNER_LIBOS,
                                      &bench_probe_page_map_unmap, code_len,
                                      &params, sizeof(params),
                                      BENCH_DATA_LEN - sizeof(params),
                                      &img) == VMM_OK);
    CU_ASSERT(build_ok);

    uint64_t cycles = 0;
    int ok = 0;
    if (build_ok) {
        libos_test_launch_result_t run = libos_test_launch(&img);
        libos_destroy_image(PAGE_OWNER_LIBOS, &img);

        ok = (run.switch_in_status == VMM_OK &&
             run.switch_out_status == VMM_OK && run.fault_count == 0);
        CU_ASSERT(ok);
        cycles = run.result;
    }

    CU_ASSERT_EQUAL(exo_syscall_dispatch(EXO_SYS_PAGE_FREE, paddr, 0, 0,
                                        0, 0, 0), 0);

    if (ok) {
        report("exo_page_map+unmap   (#2/#3)", cycles, 2 * PAIR_ITERATIONS);
    }
}

int syscall_bench_suite_cleanup(void)
{
    libos_test_teardown_owner(TEST_OWNER_SYSCALL_BENCH);
    if (saved_libos_pml4 != 0) {
        vmm_bind_address_space(PAGE_OWNER_LIBOS, saved_libos_pml4);
    }
    return 0;
}

void suite_syscall_bench_tests(CU_pSuite s)
{
    CU_add_test(s, "exo_get_ticks round-trip cycles", test_bench_get_ticks);
    CU_add_test(s, "exo_kbd_poll round-trip cycles", test_bench_kbd_poll);
    CU_add_test(s, "exo_serial_write round-trip cycles",
               test_bench_serial_write);
    CU_add_test(s, "exo_fb_acquire round-trip cycles", test_bench_fb_acquire);
    CU_add_test(s, "dispatcher -ENOSYS floor round-trip cycles",
               test_bench_dispatcher_floor);
    CU_add_test(s, "exo_page_alloc+free round-trip cycles",
               test_bench_page_alloc_free);
    CU_add_test(s, "exo_page_map+unmap round-trip cycles",
               test_bench_page_map_unmap);
}
