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
#include "libos_fs/exo_file.h"
#include "libos_fs/exo_file_stdio.h"
#include "libos_fs/exofs.h"
#include "libos_wad_params.h"
#include "stdio.h"

#ifdef EXO_KERNEL
#include "syscall.h"
#endif

#include <stdint.h>

/*
 * The IWAD's name on the program volume (SCRUM-66, SCRUM-190). Must be a name
 * src/doom/d_iwad.c actually searches for. It is LIBOS_WAD_FILE_NAME
 * (src/libos_wad_params.h) rather than its own literal: that is also the name
 * docker/scripts/build.sh writes the WAD under on ExoFS, and that header's own
 * comment explains why one #define beats several strings.
 */
#define DOOM_IWAD_NAME LIBOS_WAD_FILE_NAME

/* The system disk: the program volume is on device 0 (src/blkdev.h). */
#define DG_SYSTEM_DEV 0u

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
 * Bind and mount the program volume (SCRUM-75).
 *
 * The IWAD is a file on the program volume, the one `install` copies it to,
 * and Doom reads it with ordinary fopen/fread/fseek like any other program:
 * no preloaded buffer, no mapping. The manifest's EXO_PKG_WANT_PROGRAM_RO
 * makes the kernel assign that extent to this program (read-only, shared with
 * the shell, which binds the same sectors read-write for its own commands),
 * exo_disk_extent (#37) says where it is, and exo_disk_acquire (#26) binds
 * it. ExoFS is then mounted over it and the FILE* backend installed.
 *
 * Mounted once and left mounted for the life of the program. Anything else
 * the shell wants from that volume while Doom runs has to wait: the volume's
 * read-only binding cannot coexist with the shell's read-write one.
 *
 * Returns 0 or a DOOM_WAD_* code: ENOENT for "no volume to read" (not
 * granted, not mounted), which sends the reader to the manifest and to
 * `install` rather than at the file.
 */
static int dg_mount_program_volume(void)
{
    exo_diskextent_t ext;
    int64_t          rc;

#ifdef EXO_KERNEL
    rc = exo_syscall_dispatch(EXO_SYS_DISK_EXTENT, (uint64_t)(uintptr_t)&ext,
                              0, 0, 0, 0, 0);
#else
    rc = exo_disk_extent(&ext);
#endif
    if (rc != 0) {
        printf("DG_Init: no program volume was granted to this program "
               "(manifest lacks progvol?): %d\n", (int)rc);
        return DOOM_WAD_ENOENT;
    }

#ifdef EXO_KERNEL
    rc = exo_syscall_dispatch(EXO_SYS_DISK_ACQUIRE, DG_SYSTEM_DEV, ext.base,
                              ext.sectors, 0, 0, 0);
#else
    rc = exo_disk_acquire(DG_SYSTEM_DEV, ext.base, ext.sectors);
#endif
    if (rc != 0) {
        printf("DG_Init: could not bind the program volume (LBA %u +%u): %d "
               "-- is the shell using it?\n",
               (unsigned)ext.base, (unsigned)ext.sectors, (int)rc);
        return DOOM_WAD_ENOENT;
    }

    exofs_vol_t *vol = NULL;
    rc = exofs_mount(DG_SYSTEM_DEV, ext.base, &vol);
    if (rc != 0) {
        printf("DG_Init: program volume will not mount: %d\n", (int)rc);
        return DOOM_WAD_ENOENT;
    }

    exo_file_set_volume(vol);
    exo_file_stdio_install();
    return 0;
}

int dg_check_iwad(const char *name)
{
    uint8_t hdr[12];
    FILE   *f;
    size_t  got;
    long    size;
    int     is_iwad;

    f = fopen(name, "rb");
    if (f == NULL) {
        printf("DG_Init: '%s' is not on the program volume (run `install %s`)"
               "\n", name, name);
        return DOOM_WAD_ENOENT;
    }

    got  = fread(hdr, 1, sizeof hdr, f);
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fclose(f);

    /* Only the header: the directory is W_AddFile's to read and to diagnose,
     * with engine context this layer lacks (see DG_Init). */
    if (got != sizeof hdr || hdr[1] != 'W' || hdr[2] != 'A' || hdr[3] != 'D' ||
        (hdr[0] != 'I' && hdr[0] != 'P')) {
        printf("DG_Init: '%s' is not a WAD (bad magic or short file)\n", name);
        return DOOM_WAD_EINVAL;
    }
    if (size < 0 || (unsigned long)size > DOOM_WAD_MAX_BYTES) {
        printf("DG_Init: '%s' is too large to be a WAD Doom can use\n", name);
        return DOOM_WAD_ETOOBIG;
    }

    is_iwad = hdr[0] == 'I';
    printf("DG_Init: WAD found -- %s, %u bytes, %u lumps\n",
           is_iwad ? "IWAD" : "PWAD", (unsigned)size,
           (unsigned)(hdr[4] | (hdr[5] << 8) | (hdr[6] << 16) |
                      ((unsigned)hdr[7] << 24)));
    if (!is_iwad) {
        printf("DG_Init: warning -- this is a PWAD (a patch), not a base "
               "IWAD; expect W_Init to fail on a missing lump.\n");
    }
    return DOOM_WAD_OK;
}

/*
 * DG_Init -- SCRUM-73, reworked by SCRUM-75.
 *
 * doomgeneric calls this once from doomgeneric_Create(), immediately before
 * D_DoomMain(), whose first act is the IWAD search (d_iwad.c: M_FileExists ->
 * fopen) and then W_AddFile (w_file_stdc.c: fopen/fseek/fread). This is where
 * those fopen calls become able to find anything: DG_Init mounts the program
 * volume and installs the FILE* backend over it, then confirms the IWAD
 * really is there and really starts like a WAD, and says so on serial.
 *
 * Until SCRUM-75 there was nothing to read: the kernel preloaded the whole
 * IWAD into a buffer, mapped it, and DG_Init registered those bytes as a
 * fake file. That path (g_doom_params, exo_file_register_blob) is gone; this
 * program reads what is on the volume.
 *
 * It reports rather than halts. A missing or malformed IWAD is what Doom's
 * own startup is built to diagnose, with more engine context than this layer
 * has, and a halt here would pre-empt it. It also means the real function is
 * drivable from a ring-0 test: dg_init_result() carries the outcome, and
 * there is no #ifdef splitting the tested path from the shipped one.
 */
void DG_Init(void)
{
    int rc = dg_mount_program_volume();

    if (rc == 0) {
        rc = dg_check_iwad(DOOM_IWAD_NAME);
    }
    g_dg_init_result = rc;
}
