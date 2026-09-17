/*
 * doomgeneric_exo.c — ExoDoom's doomgeneric platform layer.
 * SCRUM-74: DG_GetTicksMs and DG_SleepMs over exo_get_ticks (#5, SCRUM-33).
 * SCRUM-73: DG_Init, mounting the IWAD the kernel mapped for us.
 *
 * doomgeneric expects one platform file per port (doomgeneric_sdl.c,
 * doomgeneric_xlib.c, ...) supplying the same six callbacks. This is
 * ExoDoom's, and it currently implements three of the six. The other three --
 * DG_DrawFrame, DG_GetKey, DG_SetWindowTitle -- belong to their own tickets
 * (Sprint 8) and are deliberately absent rather than stubbed: nothing links
 * src/doom/ yet (see docs/libc_audit.md), so an undefined reference is the
 * honest placeholder and a stub that returns a plausible value is not.
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

#include "doom_wad.h"
#include "libos_wad_params.h"
#include "stdio.h"

#ifdef EXO_KERNEL
#include "syscall.h"
#endif

#include <stdint.h>

/*
 * g_doom_params -- where the kernel tells this LibOS about the WAD.
 *
 * SCRUM-175's generalized params hand-off, the same one
 * src/libos_wad_viewer/libos_wad_viewer.c uses and reusing its struct
 * (src/libos_wad_params.h) rather than declaring a second type with the same
 * two fields: the kernel-side launcher for a Doom LibOS will patch this with
 * the identical libos_launch_patch_params() call.
 *
 * Non-zero-initialized on purpose. A zero initializer would put this in .bss,
 * where GCC stores no real bytes and libos_launch_patch_params() -- which
 * writes through img.data_paddrs[0], i.e. offset 0 of the .data blob -- would
 * have nothing to overwrite. The sentinel doubles as the "nobody patched
 * this" value DG_Init reports below, so an unpatched launch fails loudly at
 * DG_Init rather than dereferencing whatever address 0 happens to be.
 *
 * For the patch to land, this must stay (a) the first global declared in this
 * file and (b) this file first in build.sh's source list for the eventual
 * Doom link target -- the same two requirements .text.entry placement already
 * imposes. See libos_launch_patch_params()'s own comment in
 * src/libos_launch.h.
 */
#define DOOM_PARAMS_UNSET 0xFFFFFFFFFFFFFFFFULL

libos_wad_params_t g_doom_params = {
    .wad_vaddr = DOOM_PARAMS_UNSET,
    .wad_size  = DOOM_PARAMS_UNSET,
};

/*
 * DG_Init's outcome, since its signature has nowhere to return one.
 *
 * DG_INIT_NOT_RUN until DG_Init() is called, then a DOOM_WAD_* code. See
 * dg_init_result() in src/doomgeneric_exo.h for why DG_Init reports rather
 * than halts.
 */
static int g_dg_init_result = DG_INIT_NOT_RUN;

int dg_init_result(void)
{
    return g_dg_init_result;
}

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
 * anything that doesn't know it, and touches no SSE state at all despite its
 * SSE2-era name -- so it was safe here even before SCRUM-177 enabled SSE, and
 * is unaffected by it now.
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
 *
 * ── Cost: one syscall per iteration, not one memory read ────────────────
 *
 * Worth knowing before anyone leans on this for a long sleep. In the ring-3
 * build every exo_ticks_ms() here is a real `syscall` round trip, not a
 * load, so the loop is far busier than the `pause` suggests.
 *
 * It is worse than the instruction count implies, too: `syscall`'s FMASK
 * clears IF (SYSCALL_FMASK in src/syscall.c), so each iteration spends its
 * whole syscall duration with interrupts disabled -- unable to take the very
 * IRQ0 it is waiting on. The tick is not lost, since it stays pending at the
 * PIC and fires once sysret restores IF, so this costs CPU and jitter rather
 * than correctness.
 *
 * It stays this way because there is nothing better to reach for: `hlt` is
 * privileged, and no blocking sleep syscall exists. The real fixes are both
 * new kernel surface and belong to their own ticket -- a blocking exo_sleep,
 * or a shared tick page mapped read-only into the LibOS so the common case
 * is a load instead of a trap.
 *
 * In practice Doom's own demand is small: the hot path asks for 1 ms
 * (d_loop.c's TryRunTics and d_main.c's D_DoomLoop both call I_Sleep(1)), so
 * at a 1000 Hz PIT each sleep waits at most one tick. The single large call,
 * I_Sleep(100) at d_loop.c:334, is in the netgame startup wait, which a
 * single-player boot never reaches.
 */
void DG_SleepMs(uint32_t ms)
{
    uint32_t start = exo_ticks_ms();

    while ((uint32_t)(exo_ticks_ms() - start) < ms) {
        __asm__ volatile("pause" ::: "memory");
    }
}

/*
 * DG_Init — SCRUM-73.
 *
 * doomgeneric calls this once from doomgeneric_Create(), after allocating
 * DG_ScreenBuffer and immediately before D_DoomMain(). On an SDL port it is
 * where the window gets created. Here it is where the IWAD becomes usable,
 * which has to happen before D_DoomMain() because the first thing that runs
 * inside it is the IWAD search (d_iwad.c) and then W_AddFile.
 *
 * ── The ticket summary says "via exo_fb_map"; that is a mis-filing ─────
 *
 * There is no exo_fb_map syscall and there never was -- framebuffer mapping
 * is exo_fb_acquire (#4) plus exo_page_map (#2), composed LibOS-side by
 * libos_fb_map() (src/libos_fb.c, SCRUM-36), and none of it has anything to
 * do with an IWAD. The Jira description flags this on the ticket itself. The
 * mapping that matters here is the multiboot module one: SCRUM-16's
 * mmap_find_module() finds freedoom2.wad's physical range, and
 * libos_map_wad() (src/libos_wad_map.c) maps those existing pages read-only
 * into this LibOS's address space at LIBOS_WAD_VADDR before it is ever
 * entered.
 *
 * So there is no loading to do in the read-a-file sense, and nowhere to load
 * 28 MB to if there were. The bytes are already mapped. DG_Init's job is to
 * confirm that what is mapped is really a WAD, record it, and say so on
 * serial -- which is exactly the acceptance criterion: "Doom init sequence
 * runs; serial shows WAD loaded without I_Error".
 *
 * ── Why a failure here reports rather than halts ───────────────────────
 *
 * Two reasons, and the second is the one that actually decides it.
 *
 * Mechanically, it cannot call I_Error: that lives in src/doom/i_system.c,
 * nothing under src/doom/ links into build/exodoom yet
 * (docs/libc_audit.md), and this file IS linked -- so the call would be an
 * undefined reference that breaks the kernel link outright.
 *
 * But it should not halt on its own either. A missing or malformed IWAD is
 * precisely what Doom's own startup is built to diagnose: D_DoomMain runs
 * d_iwad.c's search and then W_AddFile, and that path raises I_Error with
 * the engine's own context about which lump or which file it wanted.
 * Stopping the world inside DG_Init would pre-empt that for no gain --
 * the diagnosis below is already on serial by then, and it is strictly more
 * specific than I_Error's would be (which of the four distinct failures
 * happened, plus the address and length it was handed).
 *
 * So DG_Init records its outcome in dg_init_result() and returns, which is
 * also what makes it drivable from a ring-0 unit test: there is no #ifdef
 * here splitting "halt in ring 3, return under test", and therefore no
 * chance of the tested path and the shipped path diverging.
 */
void DG_Init(void)
{
    const doom_wad_t *wad;
    int               rc;

    /*
     * The unpatched case gets its own message, because it is a launcher bug
     * rather than a WAD problem and the two send you to completely different
     * files. If g_doom_params still holds its sentinel, nobody called
     * libos_launch_patch_params() -- see the global's own comment above for
     * the two ordering requirements that make that patch land.
     */
    if (g_doom_params.wad_vaddr == DOOM_PARAMS_UNSET ||
        g_doom_params.wad_size == DOOM_PARAMS_UNSET) {
        printf("DG_Init: WAD params were never patched in "
               "(libos_launch_patch_params not called, or this TU is not "
               "first in the link order).\n");
        g_dg_init_result = DOOM_WAD_ENOENT;
        return;
    }

    /*
     * Truncated to uint32_t deliberately: wad_t and the WAD format itself are
     * 32-bit throughout (src/wad.h -- a WAD directory stores 32-bit file
     * offsets, so a WAD over 4 GiB cannot address its own lumps). The guard
     * is here rather than left to the cast so an absurd size is reported as
     * an absurd size instead of silently wrapping into a plausible one.
     */
    if (g_doom_params.wad_size > DOOM_WAD_MAX_BYTES) {
        /*
         * Printed as two zero-padded 32-bit halves, not as a decimal.
         * kvprintf (src/stdio.c) has no length modifiers, so there is no
         * %llu -- and (unsigned)wad_size would truncate to the low 32 bits,
         * which is exactly the wrap this branch exists to catch. A size of
         * 0x1_0000_0100 would have reported itself as "256 bytes exceeds
         * the 67108864 byte window", a sentence that sends the reader off
         * after the wrong thing entirely.
         */
        printf("DG_Init: WAD size 0x%08x%08x bytes exceeds the %u byte "
               "window.\n",
               (unsigned)(g_doom_params.wad_size >> 32),
               (unsigned)(g_doom_params.wad_size & 0xFFFFFFFFu),
               (unsigned)DOOM_WAD_MAX_BYTES);
        g_dg_init_result = DOOM_WAD_ETOOBIG;
        return;
    }

    rc = doom_wad_mount((const void *)(uintptr_t)g_doom_params.wad_vaddr,
                        (uint32_t)g_doom_params.wad_size);

    if (rc != DOOM_WAD_OK) {
        /* %u is safe here: wad_size already passed the check above, so it
         * is at most DOOM_WAD_MAX_BYTES and cannot lose bits. */
        printf("DG_Init: WAD mount failed at %p (%u bytes): %s\n",
               (void *)(uintptr_t)g_doom_params.wad_vaddr,
               (unsigned)g_doom_params.wad_size,
               doom_wad_strerror(rc));
        g_dg_init_result = rc;
        return;
    }

    g_dg_init_result = DOOM_WAD_OK;
    wad              = doom_wad_mounted();

    /*
     * The line the acceptance criterion asks for. It names the magic
     * explicitly rather than just saying "WAD loaded": an IWAD is a complete
     * game and a PWAD is a patch over one, and shipping the latter as the
     * former is a mistake this project has already made once -- it surfaced
     * deep inside W_Init/Z_Malloc/R_Init instead of at the four bytes that
     * actually said so (docs/architecture.md sec7, SCRUM-164). Four bytes of
     * output here is cheap next to that debugging session.
     */
    printf("DG_Init: WAD loaded -- %s, %u lumps, %u bytes at %p\n",
           wad->is_iwad ? "IWAD" : "PWAD",
           (unsigned)wad->numlumps,
           (unsigned)wad->size,
           (void *)(uintptr_t)g_doom_params.wad_vaddr);

    if (!wad->is_iwad) {
        /* Not fatal -- Doom's own d_iwad.c decides whether it has a playable
         * game. Worth saying out loud, though, since a PWAD here means the
         * base game is missing and the failure will land much later and much
         * less legibly. */
        printf("DG_Init: warning -- this is a PWAD (a patch), not a base "
               "IWAD; expect W_Init to fail on a missing lump.\n");
    }
}
