/*
 * doomgeneric_exo.c — ExoDoom's doomgeneric platform layer: timer half.
 * SCRUM-74: DG_GetTicksMs and DG_SleepMs over exo_get_ticks (#5, SCRUM-33).
 *
 * doomgeneric expects one platform file per port (doomgeneric_sdl.c,
 * doomgeneric_xlib.c, ...) supplying the same six callbacks. This is
 * ExoDoom's, and it currently implements two of the six. The other four --
 * DG_Init (SCRUM-73), DG_DrawFrame, DG_GetKey, DG_SetWindowTitle -- belong to
 * their own tickets and are deliberately absent rather than stubbed: nothing
 * links src/doom/ yet (see docs/libc_audit.md), so an undefined reference is
 * the honest placeholder and a stub that returns a plausible value is not.
 *
 * It lives in src/ rather than src/doom/ because src/doom/ is vendored
 * verbatim (SCRUM-63) and a re-vendor should stay a clean drop-in. It is
 * still built into the kernel by the C-source glob in build.sh's step 3, for
 * the same reason src/libos_heap.c and src/libos_page_alloc.c are: that is
 * what lets tests/kernel/test_doomgeneric_timer_k.c drive these functions
 * from ring 0 without a full LibOS launch.
 *
 * ── Which syscall path, and why it differs per build ────────────────────
 *
 * Same #ifdef EXO_KERNEL split as src/libos_page_alloc.c, for the same
 * reason spelled out in that file's header:
 *
 *   - NOT EXO_KERNEL (a real ring-3 LibOS link target): call the ordinary
 *     exo_get_ticks() stub from src/exo_syscall.h. The code is already at
 *     CPL 3, having arrived via libos_enter(), so the stub's sysretq keeps
 *     it exactly where it was.
 *   - EXO_KERNEL (built into the kernel, driven from ring 0 by the unit
 *     test): call exo_syscall_dispatch() directly. A `syscall` instruction
 *     executed from ring 0 would sysretq a CPL-0 caller down to CPL 3,
 *     which is not a thing to do inside a test. The plain C call exercises
 *     the same handler, which is what these tests are about.
 *
 * Either way the value comes from sys_get_ticks() in src/syscall_pit.c, so
 * there is no second time source to disagree with the first.
 */

#include "doomgeneric_exo.h"
#include "exo_syscall.h"

#ifdef EXO_KERNEL
#include "syscall.h"
#endif

#include <stdint.h>

/*
 * The one place the tick value enters this file.
 *
 * exo_get_ticks is documented as never failing (src/exo_syscall.h #5), and
 * the handler returns kernel_get_ticks_ms(), a uint32_t -- so a negative
 * result is not a clock reading, it is the dispatcher saying the syscall
 * isn't bound (-EXO_ENOSYS). Casting that straight to uint32_t would hand
 * Doom something near 4.29 billion ms and make I_GetTime believe ~49 days
 * had elapsed, which would surface as wildly broken timing rather than as
 * "the timer syscall is missing". Reporting 0 instead keeps a missing
 * syscall looking like a stopped clock, which is what it is.
 */
static uint32_t exo_ticks_ms(void)
{
#ifdef EXO_KERNEL
    int64_t t = exo_syscall_dispatch(EXO_SYS_GET_TICKS, 0, 0, 0, 0, 0, 0);
#else
    int64_t t = exo_get_ticks();
#endif

    if (t < 0) {
        return 0;
    }

    return (uint32_t)t;
}

uint32_t DG_GetTicksMs(void)
{
    return exo_ticks_ms();
}

/*
 * Busy-wait until `ms` milliseconds of PIT time have passed.
 *
 * The subtraction is deliberately done in uint32_t so the loop stays correct
 * across the counter's ~49.7-day wrap: (now - start) is the true elapsed
 * count modulo 2^32 even when `now` has wrapped past `start`. This mirrors
 * kernel_sleep_ms (src/sleep.c) and Doom's own I_GetTime, which does the same
 * thing with its `basetime`.
 *
 * `pause` rather than `hlt`: hlt is privileged, so it would #GP the moment
 * this runs where it is actually meant to run, in ring 3. pause is legal at
 * any CPL, encodes as `rep nop` (F3 90) so it degrades to a plain nop on
 * anything that doesn't know it, and needs none of the SSE state that is not
 * enabled yet (see docs/libc_audit.md sec5) despite its SSE2-era name.
 *
 * ── Precondition: the clock has to be running ──────────────────────────
 *
 * This spins on a counter it cannot advance itself, so it returns only if
 * something else is advancing it -- IRQ0 from the PIT, with interrupts
 * enabled. On a normal boot that holds: kernel_main does pit_init(1000) and
 * `sti` before anything Doom-shaped runs.
 *
 * Under -DTESTING it does NOT hold. kernel_main deliberately performs no
 * blanket `sti` before run_tests() (see the comment at that branch: several
 * suites drive ring-3 faults with RFLAGS.IF hardcoded clear, so ambient IF
 * is not something any suite may assume). A DG_SleepMs(n>0) called with
 * interrupts off would never return and would surface as a CI timeout rather
 * than a failing assertion. tests/kernel/test_doomgeneric_timer_k.c
 * therefore enables interrupts around its own waits and restores them after,
 * exactly as test_syscall_pit_k.c already does.
 *
 * No internal bailout guards this. A deadline check would need a second,
 * independent time source to measure the deadline against -- and if one
 * existed, DG_SleepMs would be using it instead. Documenting the
 * precondition is the honest option; inventing an escape hatch that silently
 * returns early would turn a hang into wrong game timing, which is harder to
 * notice and harder to debug.
 */
void DG_SleepMs(uint32_t ms)
{
    uint32_t start = exo_ticks_ms();

    while ((uint32_t)(exo_ticks_ms() - start) < ms) {
        __asm__ volatile("pause" ::: "memory");
    }
}
