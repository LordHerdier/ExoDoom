/*
 * libos_top.c — live resource dashboard LibOS (SCRUM-236), ring 3.
 *
 * Redraws about four times a second, in place: the console is cleared once
 * at start and every later frame rewrites each line padded to the full row
 * width, so the screen never goes blank between frames (clearing first made
 * the compositor's copy catch a black frame -- visible as flicker). Shows uptime (exo_get_ticks #5), the page
 * pool (exo_memstat #22), what is still free (exo_resstat #38), the system
 * disk (exo_diskstat #31) and one row per live context (exo_pslist #23, then
 * exo_grantstat #39 for each) showing its name, pages, whether it holds the
 * framebuffer, its disk extent, the sound voices it holds and the music
 * stream. All of it is read-only, unscoped introspection -- no syscall was
 * added for this; see the note in docs/syscall_spec.md about the global view
 * those calls give any caller.
 *
 * This is a full-screen app you Ctrl+Tab to, like the clock: the kernel runs
 * one context at a time and the compositor shows only the foreground one
 * (src/fb_compositor.h), so the dashboard cannot sit beside a running
 * program. It therefore never calls exo_yield() in its loop, for the same
 * reason libos_clock.c gives. The contexts it lists are the ones switched
 * away from, so their numbers are steady while it is on screen.
 *
 * Structure, entry-point and build conventions are libos_clock.c's: the
 * entry carries .text.entry and this file is listed first for its target in
 * docker/scripts/build.sh. No libc shim is linked in.
 */

#include "exo_syscall.h"
#include "libos_fb.h"
#include "fb.h"
#include "fb_console.h"
#include "ps2.h"   /* ps2_key_t (KEY_Q/KEY_ESC) only */

#include <stddef.h>
#include <stdint.h>

__attribute__((section(".text.entry")))
void libos_top_main(void);

/* .data, not .rodata -- see libos_clock.c's comment on the empty-.data
 * objcopy error. */
static char dash_banner[] = "ExoDoom Resource Dashboard";
static char dash_rule[]   = "----------------------------------------------------------------------";
static char dash_hint[]   = "Q/Esc to exit";

#define DASH_REFRESH_MS   250
#define DASH_BAR_WIDTH    40

static void write_u32(fb_console_t *con, uint32_t val)
{
    if (val == 0) { fbcon_write(con, "0"); return; }
    char buf[11];
    buf[10] = '\0';
    int i = 10;
    while (val > 0) { buf[--i] = '0' + (val % 10); val /= 10; }
    fbcon_write(con, &buf[i]);
}

static void write_2digit(fb_console_t *con, uint32_t val)
{
    char buf[3] = { (char)('0' + (val / 10) % 10),
                    (char)('0' + val % 10),
                    '\0' };
    fbcon_write(con, buf);
}

static void label(fb_console_t *con, const char *text)
{
    fbcon_set_color(con, 220, 220, 220, 0, 0, 0);
    fbcon_write(con, text);
    fbcon_set_color(con, 255, 220, 80, 0, 0, 0);
}

/* Blank the rest of the current line and move to the next, so text from the
 * previous frame that this frame no longer writes is overwritten. The last
 * column is left alone: writing it would wrap and double the newline. */
static void eol(fb_console_t *con)
{
    while (con->cursor_x + 1 < con->cols) { fbcon_write(con, " "); }
    fbcon_write(con, "\n");
}

/* "[#####-----]": `used` of `total`, rounded down but never empty if any is
 * used, so a nearly-idle pool still shows something. */
static void write_bar(fb_console_t *con, uint32_t used, uint32_t total)
{
    uint32_t filled = 0;
    if (total != 0) {
        filled = (uint32_t)(((uint64_t)used * DASH_BAR_WIDTH) / total);
        if (filled == 0 && used != 0) { filled = 1; }
    }
    char bar[DASH_BAR_WIDTH + 3];
    bar[0] = '[';
    for (uint32_t i = 0; i < DASH_BAR_WIDTH; i++) {
        bar[1 + i] = i < filled ? '#' : '-';
    }
    bar[DASH_BAR_WIDTH + 1] = ']';
    bar[DASH_BAR_WIDTH + 2] = '\0';
    fbcon_set_color(con, 80, 220, 120, 0, 0, 0);
    fbcon_write(con, bar);
    eol(con);
}

static const char *state_name(uint8_t state)
{
    switch (state) {
    case 1:  return "READY  ";
    case 2:  return "RUNNING";
    case 3:  return "BLOCKED";
    default: return "?      ";
    }
}

/* Pad the current line out to column `col` (never backwards). */
static void pad_to(fb_console_t *con, uint32_t col)
{
    while (con->cursor_x < col) { fbcon_write(con, " "); }
}

static void write_name(fb_console_t *con, const char *name)
{
    /* The kernel NUL-terminates, but this is the last place to trust it. */
    char buf[CONTEXT_NAME_MAX];
    uint32_t n = 0;
    while (n < CONTEXT_NAME_MAX - 1 && name[n] != '\0') { buf[n] = name[n]; n++; }
    buf[n] = '\0';
    fbcon_write(con, n != 0 ? buf : "-");
}

static void render(fb_console_t *con, uint32_t total_seconds)
{
    /* No clear: every line below is written out to the row width. */
    con->cursor_x = 0;
    con->cursor_y = 0;

    fbcon_set_color(con, 100, 220, 255, 0, 0, 0);
    fbcon_write(con, dash_banner);
    eol(con);
    fbcon_set_color(con, 60, 60, 60, 0, 0, 0);
    fbcon_write(con, dash_rule);
    eol(con);

    label(con, "uptime  ");
    write_u32(con, total_seconds / 3600);
    fbcon_write(con, ":");
    write_2digit(con, (total_seconds / 60) % 60);
    fbcon_write(con, ":");
    write_2digit(con, total_seconds % 60);
    eol(con);

    exo_memstat_t mem;
    if (exo_memstat(&mem) == 0) {
        uint32_t used = mem.total_pages - mem.free_pages;
        label(con, "pages   ");
        write_u32(con, used);
        fbcon_write(con, " used / ");
        write_u32(con, mem.total_pages);
        fbcon_write(con, " (");
        write_u32(con, mem.free_pages);
        fbcon_write(con, " free)");
        eol(con);
        write_bar(con, used, mem.total_pages);
        label(con, "kernel  ");
        write_u32(con, mem.kernel_pages);
        fbcon_write(con, "   libos ");
        write_u32(con, mem.libos_pages);
        eol(con);
    } else {
        label(con, "pages   unavailable");
        eol(con);
        eol(con);
        eol(con);
    }

    exo_resstat_t res;
    label(con, "free    ");
    if (exo_resstat(&res) == 0) {
        fbcon_write(con, "contexts ");
        write_u32(con, res.free_contexts);
        fbcon_write(con, "   voices ");
        write_u32(con, res.sound_voices_free);
        fbcon_write(con, "   framebuffer ");
        fbcon_write(con, res.fb_held ? "held" : "free");
    } else {
        fbcon_write(con, "unavailable");
    }
    eol(con);

    exo_diskstat_t disk;
    label(con, "disk 0  ");
    if (exo_diskstat(0, &disk) == 0 && disk.present) {
        write_u32(con, disk.total_sectors);
        fbcon_write(con, " sectors");
    } else {
        fbcon_write(con, "not present");
    }
    eol(con);

    fbcon_set_color(con, 60, 60, 60, 0, 0, 0);
    fbcon_write(con, dash_rule);
    eol(con);
    fbcon_set_color(con, 100, 220, 255, 0, 0, 0);
    fbcon_write(con, "ctx");
    pad_to(con, 5);   fbcon_write(con, "name");
    pad_to(con, 21);  fbcon_write(con, "state");
    pad_to(con, 29);  fbcon_write(con, "pages");
    pad_to(con, 36);  fbcon_write(con, "fb");
    pad_to(con, 41);  fbcon_write(con, "disk extent");
    pad_to(con, 58);  fbcon_write(con, "voices");
    pad_to(con, 66);  fbcon_write(con, "music");
    eol(con);

    /* One line per context slot, live or not: a context that exited since the
     * last frame leaves a blank row instead of a stale one. */
    exo_ps_info_t procs[EXO_PSLIST_MAX];
    int64_t n = exo_pslist(procs, EXO_PSLIST_MAX);
    uint32_t shown = 0;
    for (int64_t i = 0; i < n; i++) {
        exo_grant_t g;
        /* A context can exit between pslist and grantstat: skip it. */
        if (exo_grantstat(procs[i].id, &g) != 0) { continue; }

        fbcon_set_color(con, 255, 220, 80, 0, 0, 0);
        write_u32(con, procs[i].id);
        pad_to(con, 5);   write_name(con, procs[i].name);
        pad_to(con, 21);  fbcon_write(con, state_name(procs[i].state));
        pad_to(con, 29);  write_u32(con, g.pages);
        pad_to(con, 36);  fbcon_write(con, g.fb_owner == g.ctx_id ? "yes" : "-");
        pad_to(con, 41);
        if (g.disk_sectors != 0) {
            write_u32(con, g.disk_base);
            fbcon_write(con, "+");
            write_u32(con, g.disk_sectors);
        } else {
            fbcon_write(con, "-");
        }
        pad_to(con, 58);  write_u32(con, g.sound_voices);
        pad_to(con, 66);  fbcon_write(con, g.sound_stream ? "yes" : "-");
        eol(con);
        shown++;
    }
    for (; shown < EXO_PSLIST_MAX && con->cursor_y + 2 < con->rows; shown++) {
        eol(con);
    }

    fbcon_set_color(con, 140, 140, 140, 0, 0, 0);
    fbcon_write(con, dash_hint);
    eol(con);
}

/* Same exit-or-yield shape as libos_clock.c's clock_exit_or_yield(). */
static void dash_exit_or_yield(void)
{
    exo_syscall1(EXO_SYS_EXIT, 0);
    for (;;) { exo_yield(); }
}

__attribute__((section(".text.entry")))
void libos_top_main(void)
{
    libos_fb_t libfb;
    if (libos_fb_map(&libfb) != 0 || libfb.vaddr == NULL) {
        for (;;) { }
    }

    framebuffer_t fb;
    fb_console_t con;
    if (!fb_init_bgrx8888(&fb, (uintptr_t)libfb.vaddr, libfb.pitch,
                          libfb.width, libfb.height, libfb.bpp)) {
        for (;;) { }
    }

    /* The one clear: render() overwrites in place from here on. */
    fbcon_init(&con, &fb);
    con.show_cursor = false;

    uint32_t last_slot = ~0u;

    for (;;) {
        exo_kbd_event_t ev;
        if (exo_kbd_poll(&ev) > 0 && ev.pressed &&
            (ev.key == KEY_Q || ev.key == KEY_ESC)) {
            dash_exit_or_yield();
        }

        int64_t ticks = exo_get_ticks();
        if (ticks < 0) { continue; }

        uint32_t slot = (uint32_t)ticks / DASH_REFRESH_MS;
        if (slot == last_slot) {
            /* Back off with `pause`, not another syscall: a tight loop of
             * syscalls keeps IF off long enough to lose PIT ticks (the
             * measurement is in libos_clock.c's comment). */
            for (volatile uint32_t spin = 0; spin < 20000; spin++) {
                __asm__ volatile ("pause");
            }
            continue;
        }
        last_slot = slot;

        render(&con, (uint32_t)(ticks / 1000));
    }
}
