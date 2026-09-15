/*
 * shell_main.c — the shell LibOS (SCRUM-110).
 *
 * The first ring-3 LibOS this kernel launches on a *normal* boot rather than
 * only under a test harness: kernel_main() builds this code+data blob into
 * its own fresh address space via libos_build_image() and hands off with
 * libos_enter_irq() (src/kernel.c) — the same mechanism
 * tests/kernel/libc_shim_probe/ and tests/kernel/libos_c_probe/ already
 * prove from ring 3, just never wired into a real boot before now.
 * libos_enter_irq(), not libos_enter(), because this loop needs
 * exo_kbd_poll()/exo_get_ticks() to see real IRQ-driven state instead of
 * launching with interrupts globally masked.
 *
 * Deliberately does not link the libc shim (src/stdlib.c/src/string.c/...):
 * everything here is static/stack state and a handful of local helpers, to
 * keep this comfortably inside LIBOS_LAUNCH_MAX_{CODE,DATA}_PAGES without
 * pulling in libos_heap.c/libos_page_alloc.c's allocator machinery this
 * shell has no use for.
 *
 * Command set is intentionally trivial ("help"/"clear"/"about"/"wadview") —
 * the acceptance criterion is "can type commands and see output", not a
 * real shell language. It is also bounded by what src/ps2.c's scancode
 * decoder can produce: letters, digits, a handful of punctuation, space,
 * backspace, enter. "wadview" (SCRUM-178) is the first real cooperative
 * handoff: exo_launch_wad_viewer() (src/exo_syscall.h #21) builds the WAD/
 * flat/automap viewer as a second LibOS context and switches to it, and the
 * exo_yield() call in the idle loop below is what brings control back once
 * the viewer yields in turn (src/syscall_launch.c, context_next_ready()'s
 * round robin) — no longer the documented no-op it was before this
 * context existed on a normal boot.
 *
 * shell_main() is defined FIRST in this file, ahead of every helper it
 * calls (which are only forward-declared above it), and
 * docker/scripts/build.sh's build_ring3_link_target call for this target
 * passes -fno-toplevel-reorder: libos_build_image() always treats byte 0
 * of the linked code blob as the entry point, with no ELF symbol lookup, so
 * whichever function GCC places first in this object's .text becomes the
 * real entry. -fno-toplevel-reorder is the actual guarantee (source order
 * would otherwise be just a convention GCC's default -O2 reordering pass is
 * free to ignore; -fno-reorder-functions alone does not work here, since
 * that flag only governs hot/cold partitioning, not toplevel reordering);
 * keeping shell_main() textually first as well is defense-in-depth
 * documentation of that invariant for this file specifically. See
 * probe_cflags's own comment in build.sh for how this was discovered.
 */

#include "exo_syscall.h"
#include "fb.h"
#include "fb_console.h"
#include "libos_fb.h"
#include "ps2.h"

#define SHELL_CMD_BUF_LEN 64

/* .data, not string literals (which would land in .rodata, merged into the
 * read-only code blob) -- keeps this target's .data section non-empty.
 * build_ring3_link_target's objcopy step (docker/scripts/build.sh) errors
 * out on a fully empty .data extraction, and this shell has no other
 * writable global state to keep it non-empty otherwise. */
static char shell_banner[] = "ExoDoom Shell\n";
static char shell_help_text[] = "type 'help' for a list of commands\n";
static char shell_prompt[] = "exodoom> ";
static char shell_commands_text[] = "commands: help, clear, about, wadview\n";
static char shell_about_text[] = "ExoDoom shell LibOS -- SCRUM-110\n";
static char shell_unknown_prefix[] = "unknown command: ";
static char shell_wadview_fail_text[] = "wadview: launch failed\n";

static int str_eq(const char *a, const char *b);
static char shell_key_to_ascii(uint8_t key, uint8_t modifiers);
static void shell_backspace(fb_console_t *con);
static void shell_print_prompt(fb_console_t *con);
static void shell_run_command(fb_console_t *con, const char *line);

void shell_main(void) {
    libos_fb_t fb;
    if (libos_fb_map(&fb) != 0) {
        /* Nothing to render to -- idle forever rather than fault. */
        for (;;) {
            exo_yield();
        }
    }

    framebuffer_t raw_fb;
    fb_console_t con;
    if (!fb_init_bgrx8888(&raw_fb, (uintptr_t)fb.vaddr, fb.pitch, fb.width,
                          fb.height, fb.bpp) ||
        !fbcon_init(&con, &raw_fb)) {
        for (;;) {
            exo_yield();
        }
    }

    fbcon_set_color(&con, 100, 220, 255, 0, 0, 0);
    fbcon_write(&con, shell_banner);
    fbcon_set_color(&con, 220, 220, 220, 0, 0, 0);
    fbcon_write(&con, shell_help_text);
    shell_print_prompt(&con);

    static char cmd_buf[SHELL_CMD_BUF_LEN];
    int cmd_len = 0;

    for (;;) {
        exo_kbd_event_t ev;

        while (exo_kbd_poll(&ev) == 1) {
            if (!ev.pressed) {
                continue;
            }

            if (ev.key == KEY_ENTER) {
                cmd_buf[cmd_len] = '\0';
                fbcon_write(&con, "\n");
                shell_run_command(&con, cmd_buf);
                shell_print_prompt(&con);
                cmd_len = 0;
                continue;
            }

            if (ev.key == KEY_BACKSPACE) {
                if (cmd_len > 0) {
                    cmd_len--;
                    shell_backspace(&con);
                }
                continue;
            }

            char c = shell_key_to_ascii(ev.key, ev.modifiers);
            if (c != 0 && cmd_len < SHELL_CMD_BUF_LEN - 1) {
                cmd_buf[cmd_len++] = c;
                fbcon_putc(&con, c);
            }
        }

        exo_yield();
    }
}

static int str_eq(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* ps2_key_t -> ASCII. Returns 0 for a key with no printable mapping (arrows,
 * function keys, modifiers, enter/backspace -- handled separately by the
 * caller). Shifted digits are not mapped to their symbol row (!@#$...) --
 * src/ps2.c's scancode table has no entries for those yet, so there is
 * nothing to map to regardless. */
static char shell_key_to_ascii(uint8_t key, uint8_t modifiers) {
    int shifted = (modifiers & EXO_MOD_SHIFT) != 0;

    if (key >= KEY_A && key <= KEY_Z) {
        char c = (char)('a' + (key - KEY_A));
        return shifted ? (char)(c - 32) : c;
    }
    if (key >= KEY_1 && key <= KEY_9) {
        return (char)('1' + (key - KEY_1));
    }

    switch (key) {
    case KEY_0:         return '0';
    case KEY_SPACE:     return ' ';
    case KEY_MINUS:     return '-';
    case KEY_EQUALS:    return '=';
    case KEY_COMMA:     return ',';
    case KEY_PERIOD:    return '.';
    case KEY_SLASH:     return '/';
    case KEY_SEMICOLON: return ';';
    default:            return 0;
    }
}

/* Erase the previous character cell and move the cursor back onto it.
 * fbcon_putc() (src/fb_console.c) has no backspace handling of its own, and
 * this deliberately never backs up past the start of the current row --
 * simplest correct behavior for a single-line command prompt. */
static void shell_backspace(fb_console_t *con) {
    if (con->cursor_x == 0) {
        return;
    }
    con->cursor_x--;
    fb_fill_rect(con->fb, con->cursor_x * 8, con->cursor_y * 16, 8, 16,
                con->bg_r, con->bg_g, con->bg_b);
    fbcon_redraw_cursor(con);
}

/* Colored prompt, like a real terminal's PS1 -- distinct from both the
 * banner color and the plain-text color typed input/output use, so the
 * boundary between "shell chrome" and "your input" is visible at a glance. */
static void shell_print_prompt(fb_console_t *con) {
    fbcon_set_color(con, 90, 220, 90, 0, 0, 0);
    fbcon_write(con, shell_prompt);
    fbcon_set_color(con, 220, 220, 220, 0, 0, 0);
}

static void shell_run_command(fb_console_t *con, const char *line) {
    if (line[0] == '\0') {
        return;
    }
    if (str_eq(line, "help")) {
        fbcon_write(con, shell_commands_text);
    } else if (str_eq(line, "clear")) {
        fbcon_clear(con);
    } else if (str_eq(line, "about")) {
        fbcon_write(con, shell_about_text);
    } else if (str_eq(line, "wadview")) {
        /* Does not return until the viewer yields back to the shell
         * (src/syscall_launch.c's #21 handler, exo_syscall.h's own comment
         * on exo_launch_wad_viewer()) -- a negative return here means the
         * launch failed before ever switching away, not that the viewer
         * ran and came back. */
        int64_t rc = exo_launch_wad_viewer();
        if (rc == 0) {
            /* The viewer draws over this whole physical framebuffer (there
             * is no compositor yet -- docs/architecture.md's Sprint 12
             * roadmap tracks that separately), so on a real round trip the
             * shell's own screen is gone by the time it resumes here.
             * Clearing and letting the caller's shell_print_prompt() redraw
             * is what makes "back at the shell" a clean, stable screen
             * instead of a new prompt drawn over stale automap pixels. */
            fbcon_clear(con);
        } else {
            fbcon_write(con, shell_wadview_fail_text);
        }
    } else {
        fbcon_write(con, shell_unknown_prefix);
        fbcon_write(con, line);
        fbcon_write(con, "\n");
    }
}
