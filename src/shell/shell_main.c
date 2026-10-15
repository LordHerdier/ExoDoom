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
 * Used to not link the libc shim at all; SCRUM-202 is what changes that.
 * The filesystem commands below (mkfs/mkdir/ls/cat/rm) need the ExoFS
 * library (src/libos_fs/), whose own object files reduce to just
 * memcpy/memset/memcmp/strlen/libos_heap_alloc/_free (the allowlist
 * docker/scripts/build.sh's [3b5/7] gate asserts) -- but src/string.c also
 * defines strcasecmp/strdup (which call tolower()/malloc()) and
 * src/libos_heap.c/src/libos_page_alloc.c call printf() on their
 * diagnostic paths, so this target ends up linking the same full shim set
 * tests/kernel/libc_shim_probe/ already proves fits a ring-3 target:
 * src/stdlib.c/src/stdio.c/src/string.c/src/ctype.c/src/errno.c/
 * src/fpconv.c plus src/libos_heap.c/src/libos_page_alloc.c. This file's
 * own commands still format their own output by hand (shell_write_u32/
 * _i64 below) rather than reaching for printf. Budget is not a concern:
 * LIBOS_LAUNCH_MAX_{CODE,DATA}_PAGES are 192/192 (raised for Doom, which
 * alone uses ~97/~99 of them).
 *
 * Command set is intentionally trivial ("help"/"clear"/"about"/"wadview"/
 * "mkfs"/"mkdir"/"ls"/"cat"/"rm") — the acceptance criterion is "can type
 * commands and see output", not a real shell language. It is also bounded
 * by what src/ps2.c's scancode decoder can produce: letters, digits, a
 * handful of punctuation, space, backspace, enter. "wadview" (SCRUM-178) is
 * the first real cooperative handoff: exo_launch() (src/exo_syscall.h #21)
 * builds the WAD/flat/automap viewer as a second LibOS context and switches
 * to it.
 *
 * SCRUM-202: mkfs/mkdir/ls/cat/rm wire the ExoFS library (src/libos_fs/,
 * SCRUM-189) into this shell as its first ring-3 consumer, over a scratch
 * volume at SHELL_FS_BASE_LBA -- NOT the volume at LIBOS_WAD_DISK_BASE_LBA
 * (src/doom_wad_disk.h) that holds freedoom2.wad, which `mkfs` must never
 * touch or `doom` breaks for the rest of the boot. Disk ownership
 * (exo_disk_acquire/_release, #26/#27) is acquired, mounted, used for
 * exactly one filesystem call, and released again by shell_fs_release()
 * below -- never held across the idle loop -- so a `wadview`/`clock`/`snake`/
 * `doom` launch from this shell never contends with a held-open volume.
 *
 * SCRUM-111: the idle loop below used to call exo_yield() once per
 * iteration, on the theory that it would "bring control back once the
 * viewer yields in turn." That never actually happened -- run_automap_
 * viewer() (src/libos_wad_viewer/libos_wad_viewer.c) only ever gives up
 * control by fully exiting (exo_exit() on Q/Esc), never by yielding -- so
 * the call was always either a no-op (nothing else READY) or, once the
 * Ctrl+Tab hotkey could bring the shell to the foreground while the viewer
 * was still alive and READY, actively wrong: the shell's very next loop
 * pass would immediately yield straight back to the viewer via the same
 * round-robin, undoing the switch before a keypress could ever land here.
 * Removed for that reason -- every real hand-off away from the shell is
 * already explicit (a `wadview` launch, the viewer's own exo_exit(), or
 * Ctrl+Tab), so nothing needs the shell to volunteer control on its own.
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
#include "libos_fs/exofs.h"
#include "ps2.h"

#define SHELL_CMD_BUF_LEN 64

/*
 * The shell's own scratch ExoFS volume -- deliberately NOT
 * LIBOS_WAD_DISK_BASE_LBA (src/doom_wad_disk.h), which holds freedoom2.wad
 * and must never be reformatted out from under `doom`. src/doom_wad_disk.h's
 * own comment documents [8192, 16383] as reserved, zeroed scratch space on
 * every build/exodoom_disk.img -- the same range
 * tests/kernel/test_exofs_k.c's EXOFS_TEST_BASE_LBA/VOL_SECTORS (8192/8192)
 * already uses under TESTING=1, unused here on a normal boot. If
 * src/doom_wad_disk.h's layout ever moves, this must move with it. */
#define SHELL_FS_BASE_LBA    8192u
#define SHELL_FS_VOL_SECTORS 8192u

/* Longest line of text `cat <path> <text...>` will write in one call --
 * bounded by SHELL_CMD_BUF_LEN, since the text comes straight out of
 * cmd_buf. Not a filesystem limit. */
#define SHELL_FS_PATH_BUF_LEN SHELL_CMD_BUF_LEN

/* .data, not string literals (which would land in .rodata, merged into the
 * read-only code blob) -- keeps this target's .data section non-empty.
 * build_ring3_link_target's objcopy step (docker/scripts/build.sh) errors
 * out on a fully empty .data extraction, and this shell has no other
 * writable global state to keep it non-empty otherwise. */
static char shell_banner[] = "ExoDoom Shell\n";
static char shell_help_text[] = "type 'help' for a list of commands\n";
static char shell_prompt[] = "exodoom> ";
static char shell_commands_text[] =
    "commands: help, clear, about, wadview, clock, snake, doom, memstat,\n"
    "          pslist, mkfs, mkdir <path>, ls [path], cat <path> [text...],\n"
    "          rm <path>\n";
static char shell_about_text[] = "ExoDoom shell LibOS -- SCRUM-110\n";
static char shell_unknown_prefix[] = "unknown command: ";
static char shell_wadview_fail_text[] = "wadview: launch failed\n";
static char shell_clock_fail_text[] = "clock: launch failed\n";
static char shell_snake_fail_text[] = "snake: launch failed\n";
static char shell_doom_fail_text[] = "doom: launch failed\n";
static char shell_memstat_fail_text[] = "memstat: query failed\n";
static char shell_pslist_fail_text[] = "pslist: query failed\n";
static char shell_fs_disk_busy_text[] = "disk busy: err=";
static char shell_fs_mount_fail_text[] = "mount failed (run mkfs?): err=";
static char shell_fs_noarg_text[] = "usage: path argument required\n";
static char shell_fs_ok_text[] = "ok\n";
static char shell_fs_err_prefix[] = "error: err=";
static char shell_ls_dir_suffix[] = "/\n";
static char shell_ls_file_prefix[] = "  ";

static int str_eq(const char *a, const char *b);
static char shell_key_to_ascii(uint8_t key, uint8_t modifiers);
static void shell_backspace(fb_console_t *con);
static void shell_print_prompt(fb_console_t *con);
static void shell_run_command(fb_console_t *con, const char *line);
static void shell_write_u32(fb_console_t *con, uint32_t v);
static void shell_write_i64(fb_console_t *con, int64_t v);
static void shell_run_memstat(fb_console_t *con);
static void shell_run_pslist(fb_console_t *con);
static int shell_cmd_match(const char *line, const char *cmd, const char **rest);
static int shell_fs_acquire_and_mount(fb_console_t *con);
static void shell_fs_release(void);
static void shell_run_mkfs(fb_console_t *con);
static void shell_run_mkdir(fb_console_t *con, const char *arg);
static void shell_run_ls(fb_console_t *con, const char *arg);
static void shell_run_cat(fb_console_t *con, const char *arg);
static void shell_run_rm(fb_console_t *con, const char *arg);

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
    const char *rest;

    if (line[0] == '\0') {
        return;
    }
    if (shell_cmd_match(line, "mkfs", &rest)) {
        shell_run_mkfs(con);
    } else if (shell_cmd_match(line, "mkdir", &rest)) {
        shell_run_mkdir(con, rest);
    } else if (shell_cmd_match(line, "ls", &rest)) {
        shell_run_ls(con, rest);
    } else if (shell_cmd_match(line, "cat", &rest)) {
        shell_run_cat(con, rest);
    } else if (shell_cmd_match(line, "rm", &rest)) {
        shell_run_rm(con, rest);
    } else if (str_eq(line, "help")) {
        fbcon_write(con, shell_commands_text);
    } else if (str_eq(line, "clear")) {
        fbcon_clear(con);
    } else if (str_eq(line, "about")) {
        fbcon_write(con, shell_about_text);
    } else if (str_eq(line, "wadview")) {
        /* Does not return until the viewer yields back to the shell
         * (src/syscall_launch.c's #21 handler, exo_syscall.h's own comment
         * on exo_launch()) -- a negative return here means the
         * launch failed before ever switching away, not that the viewer
         * ran and came back. */
        int64_t rc = exo_launch(EXO_LAUNCH_APP_WAD_VIEWER);
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
    } else if (str_eq(line, "clock")) {
        /* Same calling convention as wadview above: does not return until
         * something switches back to this context (Ctrl+Tab, SCRUM-111,
         * since the clock never yields on its own -- src/libos_clock/
         * libos_clock.c's own comment on why). A negative return means the
         * launch failed before switching away. */
        int64_t rc = exo_launch(EXO_LAUNCH_APP_CLOCK);
        if (rc == 0) {
            fbcon_clear(con);
        } else {
            fbcon_write(con, shell_clock_fail_text);
        }
    } else if (str_eq(line, "snake")) {
        /* Same round-trip convention as "wadview" above: does not return
         * until Snake exits (src/libos_snake/libos_snake.c's own
         * EXO_SYS_EXIT + exo_yield() fallback) and control round-robins
         * back here. */
        int64_t rc = exo_launch(EXO_LAUNCH_APP_SNAKE);
        if (rc == 0) {
            fbcon_clear(con);
        } else {
            fbcon_write(con, shell_snake_fail_text);
        }
    } else if (str_eq(line, "doom")) {
        /* Same round-trip convention again (SCRUM-66). Doom's loop does not
         * exit on its own -- D_DoomLoop() runs forever and I_Error halts --
         * so in practice this does not come back; Ctrl+Tab (SCRUM-111) is
         * how you reach the shell again. A failure here is the launch
         * itself: no WAD module (-EXO_ENODEV), or no room for the image
         * (-EXO_ENOMEM), which is much the largest of the four.
         *
         * Running "doom" again from a different foreground context launches
         * a second, independent instance rather than replacing this one
         * (SCRUM-196) -- sys_launch() (src/syscall_launch.c) only reclaims
         * context rows that have actually exited, not ones merely switched
         * away from. Both stay live until each exits on its own (or the
         * machine reboots); `pslist` lists every live context id. There is
         * no split-screen compositor yet (SCRUM-197), so only the current
         * foreground instance is ever visible at once. */
        int64_t rc = exo_launch(EXO_LAUNCH_APP_DOOM);
        if (rc == 0) {
            fbcon_clear(con);
        } else {
            fbcon_write(con, shell_doom_fail_text);
        }
    } else if (str_eq(line, "memstat")) {
        shell_run_memstat(con);
    } else if (str_eq(line, "pslist")) {
        shell_run_pslist(con);
    } else {
        fbcon_write(con, shell_unknown_prefix);
        fbcon_write(con, line);
        fbcon_write(con, "\n");
    }
}

/* Decimal, unsigned -- the only numbers memstat/pslist ever print. The
 * shell links no libc (see this file's own top comment), so there is no
 * itoa/printf to reach for; this is the one small helper both commands
 * below share. */
static void shell_write_u32(fb_console_t *con, uint32_t v) {
    char buf[11];
    int i = 10;
    buf[10] = '\0';
    do {
        buf[--i] = (char)('0' + (v % 10));
        v /= 10;
    } while (v != 0);
    fbcon_write(con, &buf[i]);
}

/* Decimal, signed -- for printing a negative EXO_E* error code alongside
 * shell_fs_err_prefix/shell_fs_disk_busy_text/shell_fs_mount_fail_text.
 * INT64_MIN is never reached here (every EXO_E* code is a tiny negative
 * int), so negating it unconditionally is safe. */
static void shell_write_i64(fb_console_t *con, int64_t v) {
    if (v < 0) {
        fbcon_putc(con, '-');
        v = -v;
    }
    char buf[21];
    int i = 20;
    buf[20] = '\0';
    uint64_t u = (uint64_t)v;
    do {
        buf[--i] = (char)('0' + (u % 10));
        u /= 10;
    } while (u != 0);
    fbcon_write(con, &buf[i]);
}

/*
 * Match `line` against the literal command word `cmd`. On a match, *rest
 * points at whatever follows: the trailing '\0' for a bare command, or the
 * first non-space character of the argument text for "cmd <args>" (which
 * may itself be "\0" if the line was all trailing spaces). Does not match
 * "cmd" as a prefix of a longer word ("mkdirfoo" does not match "mkdir").
 */
static int shell_cmd_match(const char *line, const char *cmd, const char **rest) {
    int i = 0;
    while (cmd[i] != '\0') {
        if (line[i] != cmd[i]) {
            return 0;
        }
        i++;
    }
    if (line[i] == '\0') {
        *rest = &line[i];
        return 1;
    }
    if (line[i] != ' ') {
        return 0;
    }
    while (line[i] == ' ') {
        i++;
    }
    *rest = &line[i];
    return 1;
}

/*
 * Disk ownership policy for every filesystem command (SCRUM-202, see this
 * file's own top comment): acquire, mount, do exactly one exofs_* call,
 * unmount, release -- never held across the idle loop. This is the
 * acquire+mount half, shared by every command below; shell_fs_release() is
 * the matching unmount+release half. Prints its own diagnostic and returns
 * 0 on failure so each caller can just return.
 */
static int shell_fs_acquire_and_mount(fb_console_t *con) {
    int64_t rc = exo_disk_acquire();
    if (rc != 0) {
        fbcon_write(con, shell_fs_disk_busy_text);
        shell_write_i64(con, rc);
        fbcon_write(con, "\n");
        return 0;
    }

    int mrc = exofs_mount(SHELL_FS_BASE_LBA);
    if (mrc != 0) {
        fbcon_write(con, shell_fs_mount_fail_text);
        shell_write_i64(con, mrc);
        fbcon_write(con, "\n");
        exo_disk_release();
        return 0;
    }

    return 1;
}

static void shell_fs_release(void) {
    exofs_unmount();
    exo_disk_release();
}

/* `mkfs` -- exofs_format() on the shell's own scratch volume (decision #1,
 * this file's own top comment). No mount needed afterwards: exofs_format()
 * does not leave the volume mounted, and this command has nothing further
 * to do with it. Destroys whatever the volume held before, same as any
 * other filesystem's mkfs. */
static void shell_run_mkfs(fb_console_t *con) {
    int64_t rc = exo_disk_acquire();
    if (rc != 0) {
        fbcon_write(con, shell_fs_disk_busy_text);
        shell_write_i64(con, rc);
        fbcon_write(con, "\n");
        return;
    }

    int frc = exofs_format(SHELL_FS_BASE_LBA, SHELL_FS_VOL_SECTORS);
    exo_disk_release();

    if (frc != 0) {
        fbcon_write(con, shell_fs_err_prefix);
        shell_write_i64(con, frc);
        fbcon_write(con, "\n");
        return;
    }
    fbcon_write(con, shell_fs_ok_text);
}

/* `mkdir <path>` -- exofs_mkdir(). `arg` is already trimmed of leading
 * spaces by shell_cmd_match() and is a suffix of the null-terminated
 * command buffer, so it can be passed straight through as the path with no
 * copy. */
static void shell_run_mkdir(fb_console_t *con, const char *arg) {
    if (arg[0] == '\0') {
        fbcon_write(con, shell_fs_noarg_text);
        return;
    }
    if (!shell_fs_acquire_and_mount(con)) {
        return;
    }
    int rc = exofs_mkdir(arg);
    shell_fs_release();

    if (rc != 0) {
        fbcon_write(con, shell_fs_err_prefix);
        shell_write_i64(con, rc);
        fbcon_write(con, "\n");
        return;
    }
    fbcon_write(con, shell_fs_ok_text);
}

/* `ls [path]` -- exofs_opendir()/exofs_readdir() loop, defaulting to "/"
 * when no argument is given. Directories print with a trailing "/" so
 * mkdir'd entries are visually distinct from files, same spirit as `ls -F`. */
static void shell_run_ls(fb_console_t *con, const char *arg) {
    const char *path = (arg[0] != '\0') ? arg : "/";

    if (!shell_fs_acquire_and_mount(con)) {
        return;
    }

    exofs_dir_t dir;
    int rc = exofs_opendir(path, &dir);
    if (rc != 0) {
        shell_fs_release();
        fbcon_write(con, shell_fs_err_prefix);
        shell_write_i64(con, rc);
        fbcon_write(con, "\n");
        return;
    }

    for (;;) {
        exofs_dirinfo_t info;
        int64_t got = exofs_readdir(&dir, &info);
        if (got <= 0) {
            break;
        }
        fbcon_write(con, shell_ls_file_prefix);
        fbcon_write(con, info.name);
        if (info.attributes & EXOFS_ATTR_DIRECTORY) {
            fbcon_write(con, shell_ls_dir_suffix);
        } else {
            fbcon_write(con, " (");
            shell_write_u32(con, info.size);
            fbcon_write(con, " bytes)\n");
        }
    }

    exofs_closedir(&dir);
    shell_fs_release();
}

/*
 * `cat <path>` reads and prints the file. `cat <path> <text...>` (trailing
 * words present) creates/truncates `path` and writes `text...` as its
 * entire content instead -- the one overload this shell needs to satisfy
 * "create a file" with the exact five-command set the ticket names, rather
 * than inventing a sixth verb.
 */
static void shell_run_cat(fb_console_t *con, const char *arg) {
    char path[SHELL_FS_PATH_BUF_LEN];
    int i = 0;
    while (arg[i] != '\0' && arg[i] != ' ' && i < SHELL_FS_PATH_BUF_LEN - 1) {
        path[i] = arg[i];
        i++;
    }
    path[i] = '\0';
    if (i == 0) {
        fbcon_write(con, shell_fs_noarg_text);
        return;
    }

    const char *text = &arg[i];
    while (*text == ' ') {
        text++;
    }
    int writing = (*text != '\0');

    if (!shell_fs_acquire_and_mount(con)) {
        return;
    }

    exofs_file_t f;
    int orc = writing
        ? exofs_open(path, EXOFS_O_WRITE | EXOFS_O_CREATE | EXOFS_O_TRUNC, &f)
        : exofs_open(path, EXOFS_O_READ, &f);
    if (orc != 0) {
        shell_fs_release();
        fbcon_write(con, shell_fs_err_prefix);
        shell_write_i64(con, orc);
        fbcon_write(con, "\n");
        return;
    }

    if (writing) {
        uint32_t len = 0;
        while (text[len] != '\0') {
            len++;
        }
        int64_t wrc = exofs_write(&f, text, len);
        exofs_close(&f);
        shell_fs_release();
        if (wrc < 0) {
            fbcon_write(con, shell_fs_err_prefix);
            shell_write_i64(con, wrc);
            fbcon_write(con, "\n");
            return;
        }
        fbcon_write(con, shell_fs_ok_text);
        return;
    }

    char buf[64];
    for (;;) {
        int64_t got = exofs_read(&f, buf, sizeof(buf));
        if (got < 0) {
            fbcon_write(con, shell_fs_err_prefix);
            shell_write_i64(con, got);
            fbcon_write(con, "\n");
            break;
        }
        if (got == 0) {
            break;
        }
        for (int64_t j = 0; j < got; j++) {
            fbcon_putc(con, buf[j]);
        }
    }
    fbcon_write(con, "\n");

    exofs_close(&f);
    shell_fs_release();
}

/* `rm <path>` -- tries exofs_unlink() first; falls back to exofs_rmdir() on
 * -EXO_EISDIR, so one command removes either a file or an (empty)
 * directory, matching the ticket's five-command list (no separate rmdir). */
static void shell_run_rm(fb_console_t *con, const char *arg) {
    if (arg[0] == '\0') {
        fbcon_write(con, shell_fs_noarg_text);
        return;
    }
    if (!shell_fs_acquire_and_mount(con)) {
        return;
    }

    int rc = exofs_unlink(arg);
    if (rc == -EXO_EISDIR) {
        rc = exofs_rmdir(arg);
    }
    shell_fs_release();

    if (rc != 0) {
        fbcon_write(con, shell_fs_err_prefix);
        shell_write_i64(con, rc);
        fbcon_write(con, "\n");
        return;
    }
    fbcon_write(con, shell_fs_ok_text);
}

/* `memstat` -- exo_memstat (#22, src/syscall_stat.c). The output struct is
 * an ordinary local: the shell's own stack is always in the LibOS window
 * and mapped (libos_build_image() put it there), so the -EXO_EFAULT branch
 * below is unreachable in practice and only guards against the kernel ever
 * disagreeing with that. */
static void shell_run_memstat(fb_console_t *con) {
    exo_memstat_t stat;
    if (exo_memstat(&stat) != 0) {
        fbcon_write(con, shell_memstat_fail_text);
        return;
    }

    fbcon_write(con, "total=");
    shell_write_u32(con, stat.total_pages);
    fbcon_write(con, " free=");
    shell_write_u32(con, stat.free_pages);
    fbcon_write(con, " kernel=");
    shell_write_u32(con, stat.kernel_pages);
    fbcon_write(con, " libos=");
    shell_write_u32(con, stat.libos_pages);
    fbcon_write(con, " regions=");
    shell_write_u32(con, stat.region_count);
    fbcon_write(con, " (pages)\n");
}

/* `pslist` -- exo_pslist (#23, src/syscall_stat.c). Same stack-buffer
 * reasoning as shell_run_memstat() above for why -EXO_EFAULT is not
 * expected here either. */
static void shell_run_pslist(fb_console_t *con) {
    exo_ps_info_t procs[EXO_PSLIST_MAX];
    int64_t rc = exo_pslist(procs, EXO_PSLIST_MAX);
    if (rc < 0) {
        fbcon_write(con, shell_pslist_fail_text);
        return;
    }

    for (int64_t i = 0; i < rc; i++) {
        fbcon_write(con, "id=");
        shell_write_u32(con, procs[i].id);
        fbcon_write(con, " state=");
        switch (procs[i].state) {
        case 1: fbcon_write(con, "ready");   break;
        case 2: fbcon_write(con, "running"); break;
        case 3: fbcon_write(con, "blocked"); break;
        default: fbcon_write(con, "?");      break;
        }
        fbcon_write(con, " pages=");
        shell_write_u32(con, procs[i].page_count);
        fbcon_write(con, "\n");
    }
}
