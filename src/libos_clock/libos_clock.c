/*
 * libos_clock.c — the clock LibOS demo app (SCRUM-168), running at ring 3.
 *
 * Third real ring-3 LibOS app after the shell (SCRUM-110) and the WAD/flat/
 * automap viewer (SCRUM-178), and structured identically: a code+data blob
 * built and switched to by the shell's "clock" command
 * (src/shell/shell_main.c) via exo_launch(EXO_LAUNCH_APP_CLOCK) (#21, src/syscall_launch.c),
 * with no libc shim linked in (stays comfortably inside
 * LIBOS_LAUNCH_MAX_{CODE,DATA}_PAGES the same way the shell does).
 *
 * There is no RTC/CMOS driver in this kernel and exo_get_ticks (#5) is the
 * only clock source a LibOS has (docs/syscall_spec.md §3, src/exo_syscall.h)
 * -- so "live time display" here is a ticks-since-boot stopwatch rendered as
 * HH:MM:SS, not wall-clock time.
 *
 * Framebuffer multiplexing (SCRUM-112, docs/architecture.md §5.5) means
 * exo_fb_acquire() always hands back a private virtual framebuffer -- no
 * exclusivity to negotiate with the shell or any other live context -- and
 * Ctrl+Tab (SCRUM-111) switches which context's virtual framebuffer the
 * compositor shows without either side having to cooperate. That is also
 * why this loop never calls exo_yield(): shell_main.c's own SCRUM-111
 * comment explains why volunteering control used to be actively wrong once
 * Ctrl+Tab can bring another context to the foreground on its own -- every
 * switch away from this app happens asynchronously, mid-loop, from the IRQ1
 * handler, and execution simply resumes here whenever this context is next
 * switched back in.
 *
 * libos_clock_main() carries __attribute__((section(".text.entry"))) so it lands
 * at offset 0 of the linked .text blob regardless of source order or how
 * GCC schedules everything else -- see tests/kernel/ring3_link_target.ld.in's
 * own comment and libos_wad_viewer.c's identical convention. This file must
 * still be listed first in docker/scripts/build.sh's source list for this
 * target, matching every other ring-3 target's convention.
 *
 * Built by docker/scripts/build.sh's unconditional shell/WAD-viewer-style
 * step, not a TESTING-only target -- kernel_main never launches this
 * directly, but a normal boot's shell can. Lives in its own subdirectory so
 * step 3's general src/ compile loop never compiles it with -DEXO_KERNEL.
 */

#include "exo_syscall.h"
#include "libos_fb.h"
#include "fb.h"
#include "fb_console.h"
#include "ps2.h"   /* ps2_key_t (KEY_Q/KEY_ESC) only -- no kernel-only
                    * symbol from this header is called or linked in here. */

#include <stddef.h>
#include <stdint.h>

__attribute__((section(".text.entry")))
void libos_clock_main(void);

/* .data, not .rodata -- build_ring3_link_target's objcopy step errors on an
 * empty .data extraction, and this LibOS has no other writable global state
 * to keep it non-empty otherwise (same reasoning as shell_main.c's own
 * banner strings). */
static char clock_banner[] = "ExoDoom Clock\n";
static char clock_quit_hint[] = "Q/Esc to exit\n";

/* ---- small ring-3-only helpers (no libc shim linked into this target) -- */

static void write_u32(fb_console_t *con, uint32_t val)
{
    if (val == 0) { fbcon_write(con, "0"); return; }
    char buf[11];
    buf[10] = '\0';
    int i = 10;
    while (val > 0) { buf[--i] = '0' + (val % 10); val /= 10; }
    fbcon_write(con, &buf[i]);
}

/* Zero-padded two-digit field, for the MM/SS portions of HH:MM:SS -- callers
 * only ever pass values in [0, 59]. */
static void write_2digit(fb_console_t *con, uint32_t val)
{
    char buf[3] = { (char)('0' + (val / 10) % 10),
                    (char)('0' + val % 10),
                    '\0' };
    fbcon_write(con, buf);
}

static void render_clock_face(fb_console_t *con, framebuffer_t *fb,
                              uint32_t hours, uint32_t minutes,
                              uint32_t seconds)
{
    fb_clear(fb, 0, 0, 0);
    fbcon_init(con, fb);

    fbcon_set_color(con, 100, 220, 255, 0, 0, 0);
    fbcon_write(con, clock_banner);
    fbcon_set_color(con, 60, 60, 60, 0, 0, 0);
    fbcon_write(con, "-----------------\n");
    fbcon_set_color(con, 220, 220, 220, 0, 0, 0);
    fbcon_write(con, "uptime  ");

    fbcon_set_color(con, 255, 220, 80, 0, 0, 0);
    write_u32(con, hours);
    fbcon_write(con, ":");
    write_2digit(con, minutes);
    fbcon_write(con, ":");
    write_2digit(con, seconds);
    fbcon_write(con, "\n");

    fbcon_set_color(con, 140, 140, 140, 0, 0, 0);
    fbcon_write(con, clock_quit_hint);
}

/* Every quit path wants "exit, or yield forever if resumed anyway" -- same
 * reasoning as libos_wad_viewer.c's wad_viewer_exit_or_yield(): the raw
 * exo_syscall1(EXO_SYS_EXIT, ...) call plus a real `for(;;) { exo_yield(); }`
 * rather than the exo_exit() convenience wrapper, so GCC's noreturn
 * inference has nothing to prove unreachable at the call site. EXO_SYS_EXIT
 * (#20, src/syscall_exit.c) reclaims this context's pages and framebuffer
 * binding and hands off to whatever's next-ready (the shell, on a normal
 * boot) via the same round-robin context_next_ready()/
 * context_switch_request() pair Ctrl+Tab and exo_yield() use. */
static void clock_exit_or_yield(void)
{
    exo_syscall1(EXO_SYS_EXIT, 0);
    for (;;) { exo_yield(); }
}

/* ---- entry point --------------------------------------------------------- */

__attribute__((section(".text.entry")))
void libos_clock_main(void)
{
    libos_fb_t libfb;
    if (libos_fb_map(&libfb) != 0 || libfb.vaddr == NULL) {
        /* Nothing to render to -- idle forever rather than fault. */
        for (;;) { }
    }

    framebuffer_t fb;
    fb_console_t con;
    if (!fb_init_bgrx8888(&fb, (uintptr_t)libfb.vaddr, libfb.pitch,
                          libfb.width, libfb.height, libfb.bpp)) {
        for (;;) { }
    }

    uint32_t last_displayed_second = ~0u;

    for (;;) {
        exo_kbd_event_t ev;
        if (exo_kbd_poll(&ev) > 0 && ev.pressed &&
           (ev.key == KEY_Q || ev.key == KEY_ESC)) {
            clock_exit_or_yield();
        }

        int64_t ticks = exo_get_ticks();
        if (ticks < 0) {
            continue;
        }

        uint32_t total_seconds = (uint32_t)(ticks / 1000);
        if (total_seconds == last_displayed_second) {
            /* Back off with plain (non-syscall) spin instructions rather
             * than immediately re-issuing exo_get_ticks() -- measured on a
             * real boot, an unthrottled loop that re-enters the syscall
             * gate as fast as the CPU can go
             * measurably starves the PIT: IA32_FMASK clears IF for the
             * duration of every syscall (docs/syscall_spec.md §3.4,
             * CLAUDE.md's own note on exo_serial_write() being a DoS risk
             * against IRQs for the same reason), and with no pacing at all
             * between calls, the aggregate time IF spends off is enough to
             * lose a real fraction of the 1000 Hz PIT's edges -- this
             * exact loop, unthrottled, measured at ~83% of real speed
             * (~1.2 real seconds per displayed second). `pause` between
             * checks costs no syscall and so cannot mask IF at all, letting
             * every PIT tick land immediately; 1 kHz resolution is already
             * far finer than this display needs, so the added latency
             * before noticing a new second is imperceptible. See
             * docs/drivers/pit.md §9's "No compensation for missed ticks"
             * gotcha -- this is that gotcha, self-inflicted by polling too
             * fast rather than by a long cli section. */
            for (volatile uint32_t spin = 0; spin < 20000; spin++) {
                __asm__ volatile ("pause");
            }
            continue;
        }
        last_displayed_second = total_seconds;

        uint32_t seconds = total_seconds % 60;
        uint32_t minutes = (total_seconds / 60) % 60;
        uint32_t hours   = total_seconds / 3600;

        render_clock_face(&con, &fb, hours, minutes, seconds);

        /* Ring 3 cannot execute `hlt` -- backing off between polls (above)
         * rather than a tight spin on exo_get_ticks() is the timing
         * primitive available here. */
    }
}
