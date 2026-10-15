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
 * Command set is intentionally trivial — the acceptance criterion is "can
 * type commands and see output", not a real shell language. It is also
 * bounded by what src/ps2.c's scancode decoder can produce: letters, digits,
 * a handful of punctuation, space, backspace, enter. "wadview" (SCRUM-178)
 * is the first real cooperative handoff: exo_launch() (src/exo_syscall.h
 * #21) builds the WAD/flat/automap viewer as a second LibOS context and
 * switches to it.
 *
 * SCRUM-202: mkfs/mkdir/ls/cat/rm/cp/mv/touch/rm/cd/pwd/dd wire the ExoFS
 * library (src/libos_fs/, SCRUM-189) into this shell as its first ring-3
 * consumer, over a scratch volume at SHELL_FS_BASE_LBA -- NOT the volume at
 * LIBOS_WAD_DISK_BASE_LBA (src/doom_wad_disk.h) that holds freedoom2.wad,
 * which `mkfs` must never touch or `doom` breaks for the rest of the boot
 * (`dd` is the one deliberate exception to that rule -- see its own comment).
 * Disk ownership (exo_disk_acquire/_release, #26/#27) is acquired, mounted,
 * used for exactly one filesystem call (or, for `dd`, one raw-sector loop
 * plus one filesystem call on the same acquired disk), and released again
 * by shell_fs_release() below -- never held across the idle loop -- so a
 * `wadview`/`clock`/`snake`/`doom` launch from this shell never contends
 * with a held-open volume.
 *
 * `mv` is copy-then-delete, not a real rename -- ExoFS has no rename() yet
 * (a follow-up ticket covers adding one). `cd`/`pwd` are the one piece of
 * state ExoFS deliberately has none of (docs/filesystem.md §7: "a shell
 * that wants a cd keeps the state itself"): shell_cwd below, resolved
 * against by shell_resolve_path() for every other path-taking command.
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

#include "doom_wad_disk.h"
#include "exo_syscall.h"
#include "fb.h"
#include "fb_console.h"
#include "libos_fb.h"
#include "libos_fs/exofs.h"
#include "ps2.h"
#include "stdlib.h"

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

/* A resolved path (shell_cwd + '/' + a relative argument) can be longer than
 * any single token out of cmd_buf, so it gets its own, larger buffer. */
#define SHELL_FS_RESOLVED_BUF_LEN 128

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
    "          pslist, disk, mkfs, mkdir <path>, ls [path],\n"
    "          cat <path> [text...], rm <path>, cp <src> <dst>,\n"
    "          mv <src> <dst>, touch <path>, cd [path], pwd, echo <text...>,\n"
    "          dd read <lba> <count> <path>, dd write <path> <lba>\n";
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
static char shell_fs_usage2_text[] = "usage: <src> <dst>\n";
static char shell_disk_fail_text[] = "disk: query failed\n";
static char shell_disk_none_text[] = "disk: no drive present\n";
static char shell_dd_usage_text[] =
    "usage: dd read <lba> <count> <path> | dd write <path> <lba>\n";

/* ExoFS itself has no notion of a working directory by design
 * (docs/filesystem.md §7) -- this is that state, kept here instead. Always
 * absolute, always non-empty (shell_resolve_path() below assumes both).
 * Non-zero initializer so it lands in .data, not .bss, same reason every
 * other writable string in this file does. */
static char shell_cwd[SHELL_FS_RESOLVED_BUF_LEN] = "/";

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
static void shell_resolve_path(const char *in, char *out, size_t out_len);
static int shell_split_two(const char *rest, char *a, size_t alen,
                          char *b, size_t blen);
static size_t shell_next_token(const char **p, char *out, size_t out_len);
static int shell_fs_copy_file(const char *src, const char *dst);
static void shell_run_mkfs(fb_console_t *con);
static void shell_run_mkdir(fb_console_t *con, const char *arg);
static void shell_run_ls(fb_console_t *con, const char *arg);
static void shell_run_cat(fb_console_t *con, const char *arg);
static void shell_run_rm(fb_console_t *con, const char *arg);
static void shell_run_cp(fb_console_t *con, const char *arg);
static void shell_run_mv(fb_console_t *con, const char *arg);
static void shell_run_touch(fb_console_t *con, const char *arg);
static void shell_run_cd(fb_console_t *con, const char *arg);
static void shell_run_pwd(fb_console_t *con);
static void shell_run_echo(fb_console_t *con, const char *arg);
static void shell_run_disk(fb_console_t *con);
static void shell_run_dd(fb_console_t *con, const char *arg);
static void shell_run_dd_read(fb_console_t *con, const char *rest);
static void shell_run_dd_write(fb_console_t *con, const char *rest);

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
    } else if (shell_cmd_match(line, "cp", &rest)) {
        shell_run_cp(con, rest);
    } else if (shell_cmd_match(line, "mv", &rest)) {
        shell_run_mv(con, rest);
    } else if (shell_cmd_match(line, "touch", &rest)) {
        shell_run_touch(con, rest);
    } else if (shell_cmd_match(line, "cd", &rest)) {
        shell_run_cd(con, rest);
    } else if (str_eq(line, "pwd")) {
        shell_run_pwd(con);
    } else if (shell_cmd_match(line, "echo", &rest)) {
        shell_run_echo(con, rest);
    } else if (str_eq(line, "disk")) {
        shell_run_disk(con);
    } else if (shell_cmd_match(line, "dd", &rest)) {
        shell_run_dd(con, rest);
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
 * Resolve a possibly-relative path argument against shell_cwd. `in` starting
 * with '/' is copied verbatim (absolute); otherwise the result is
 * shell_cwd + '/' + in (skipping the extra '/' if shell_cwd already ends in
 * one, i.e. at root). Every call site passes a non-empty `in` -- each
 * command rejects an empty argument before ever reaching this.
 *
 * ExoFS's own path parser already collapses a repeated '/' and resolves '.'
 * /'..' through real directory entries (docs/filesystem.md §7), so there is
 * no need to canonicalize the result any further than this for it to work
 * correctly in exofs_* calls -- only `pwd`/`cd ..`'s own display can still
 * show an uncollapsed ".." component (shell_run_cd()'s own comment). */
static void shell_resolve_path(const char *in, char *out, size_t out_len) {
    if (in[0] == '/') {
        size_t i = 0;
        while (in[i] != '\0' && i < out_len - 1) {
            out[i] = in[i];
            i++;
        }
        out[i] = '\0';
        return;
    }

    size_t i = 0;
    while (shell_cwd[i] != '\0' && i < out_len - 1) {
        out[i] = shell_cwd[i];
        i++;
    }
    if (i > 0 && out[i - 1] != '/' && i < out_len - 1) {
        out[i++] = '/';
    }
    size_t j = 0;
    while (in[j] != '\0' && i < out_len - 1) {
        out[i++] = in[j++];
    }
    out[i] = '\0';
}

/* Extract the next whitespace-delimited token from *p into `out`, then
 * advance *p past it and any following spaces. Returns the token length (0
 * if *p was already empty/all spaces -- the caller's "missing argument"
 * case). Shared by `cp`/`mv`'s two-path split and `dd`'s numeric/path
 * arguments. */
static size_t shell_next_token(const char **p, char *out, size_t out_len) {
    const char *s = *p;
    size_t i = 0;
    while (s[i] != '\0' && s[i] != ' ' && i < out_len - 1) {
        out[i] = s[i];
        i++;
    }
    out[i] = '\0';
    s += i;
    while (*s == ' ') {
        s++;
    }
    *p = s;
    return i;
}

static int shell_split_two(const char *rest, char *a, size_t alen,
                          char *b, size_t blen) {
    const char *p = rest;
    if (shell_next_token(&p, a, alen) == 0) {
        return 0;
    }
    if (shell_next_token(&p, b, blen) == 0) {
        return 0;
    }
    return 1;
}

/* Shared by `cp` and `mv` (mv = this, then exofs_unlink(src) on success).
 * Streams through a 512-byte stack buffer rather than one allocation sized
 * to the whole file -- same shape `cat`'s read loop already uses, works for
 * any file size. Returns 0, or the first negative error either open or any
 * read/write step reports. */
static int shell_fs_copy_file(const char *src, const char *dst) {
    exofs_file_t fsrc, fdst;

    int rc = exofs_open(src, EXOFS_O_READ, &fsrc);
    if (rc != 0) {
        return rc;
    }
    rc = exofs_open(dst, EXOFS_O_WRITE | EXOFS_O_CREATE | EXOFS_O_TRUNC,
                    &fdst);
    if (rc != 0) {
        exofs_close(&fsrc);
        return rc;
    }

    uint8_t buf[512];
    int64_t got;
    while ((got = exofs_read(&fsrc, buf, sizeof(buf))) > 0) {
        int64_t wrote = exofs_write(&fdst, buf, (uint32_t)got);
        if (wrote < 0) {
            rc = (int)wrote;
            break;
        }
    }
    if (got < 0) {
        rc = (int)got;
    }

    exofs_close(&fsrc);
    exofs_close(&fdst);
    return rc;
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

/* `mkdir <path>` -- exofs_mkdir(), against `arg` resolved relative to
 * shell_cwd if it is not already absolute. */
static void shell_run_mkdir(fb_console_t *con, const char *arg) {
    if (arg[0] == '\0') {
        fbcon_write(con, shell_fs_noarg_text);
        return;
    }
    char path[SHELL_FS_RESOLVED_BUF_LEN];
    shell_resolve_path(arg, path, sizeof(path));

    if (!shell_fs_acquire_and_mount(con)) {
        return;
    }
    int rc = exofs_mkdir(path);
    shell_fs_release();

    if (rc != 0) {
        fbcon_write(con, shell_fs_err_prefix);
        shell_write_i64(con, rc);
        fbcon_write(con, "\n");
        return;
    }
    fbcon_write(con, shell_fs_ok_text);
}

/* `ls [path]` -- exofs_opendir()/exofs_readdir() loop, defaulting to
 * shell_cwd when no argument is given. Directories print with a trailing
 * "/" so mkdir'd entries are visually distinct from files, same spirit as
 * `ls -F`. */
static void shell_run_ls(fb_console_t *con, const char *arg) {
    char path[SHELL_FS_RESOLVED_BUF_LEN];
    if (arg[0] != '\0') {
        shell_resolve_path(arg, path, sizeof(path));
    } else {
        size_t i = 0;
        while (shell_cwd[i] != '\0' && i < sizeof(path) - 1) {
            path[i] = shell_cwd[i];
            i++;
        }
        path[i] = '\0';
    }

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
    char path_raw[SHELL_FS_PATH_BUF_LEN];
    int i = 0;
    while (arg[i] != '\0' && arg[i] != ' ' && i < SHELL_FS_PATH_BUF_LEN - 1) {
        path_raw[i] = arg[i];
        i++;
    }
    path_raw[i] = '\0';
    if (i == 0) {
        fbcon_write(con, shell_fs_noarg_text);
        return;
    }
    char path[SHELL_FS_RESOLVED_BUF_LEN];
    shell_resolve_path(path_raw, path, sizeof(path));

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
    char path[SHELL_FS_RESOLVED_BUF_LEN];
    shell_resolve_path(arg, path, sizeof(path));

    if (!shell_fs_acquire_and_mount(con)) {
        return;
    }

    int rc = exofs_unlink(path);
    if (rc == -EXO_EISDIR) {
        rc = exofs_rmdir(path);
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

/* `cp <src> <dst>` -- shell_fs_copy_file(), both paths resolved against
 * shell_cwd first. */
static void shell_run_cp(fb_console_t *con, const char *arg) {
    char src_raw[SHELL_FS_PATH_BUF_LEN], dst_raw[SHELL_FS_PATH_BUF_LEN];
    if (!shell_split_two(arg, src_raw, sizeof(src_raw),
                        dst_raw, sizeof(dst_raw))) {
        fbcon_write(con, shell_fs_usage2_text);
        return;
    }
    char src[SHELL_FS_RESOLVED_BUF_LEN], dst[SHELL_FS_RESOLVED_BUF_LEN];
    shell_resolve_path(src_raw, src, sizeof(src));
    shell_resolve_path(dst_raw, dst, sizeof(dst));

    if (!shell_fs_acquire_and_mount(con)) {
        return;
    }
    int rc = shell_fs_copy_file(src, dst);
    shell_fs_release();

    if (rc != 0) {
        fbcon_write(con, shell_fs_err_prefix);
        shell_write_i64(con, rc);
        fbcon_write(con, "\n");
        return;
    }
    fbcon_write(con, shell_fs_ok_text);
}

/* `mv <src> <dst>` -- copy then delete the source (this file's own top
 * comment on why: ExoFS has no rename() yet). Not atomic and files only --
 * exofs_unlink() on a directory source just answers -EXO_EISDIR, which
 * surfaces as this command's ordinary error path rather than a special
 * case. */
static void shell_run_mv(fb_console_t *con, const char *arg) {
    char src_raw[SHELL_FS_PATH_BUF_LEN], dst_raw[SHELL_FS_PATH_BUF_LEN];
    if (!shell_split_two(arg, src_raw, sizeof(src_raw),
                        dst_raw, sizeof(dst_raw))) {
        fbcon_write(con, shell_fs_usage2_text);
        return;
    }
    char src[SHELL_FS_RESOLVED_BUF_LEN], dst[SHELL_FS_RESOLVED_BUF_LEN];
    shell_resolve_path(src_raw, src, sizeof(src));
    shell_resolve_path(dst_raw, dst, sizeof(dst));

    if (!shell_fs_acquire_and_mount(con)) {
        return;
    }
    int rc = shell_fs_copy_file(src, dst);
    if (rc == 0) {
        rc = exofs_unlink(src);
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

/* `touch <path>` -- create an empty file if absent; a true no-op if it
 * already exists. EXOFS_O_CREATE without EXOFS_O_TRUNC only triggers
 * creation on -EXO_ENOENT (src/libos_fs/exofs_file.c), and exofs_close()
 * skips the directory-entry rewrite entirely when nothing was written
 * (its `dirty` flag), so an existing file's size/content survive
 * untouched. */
static void shell_run_touch(fb_console_t *con, const char *arg) {
    if (arg[0] == '\0') {
        fbcon_write(con, shell_fs_noarg_text);
        return;
    }
    char path[SHELL_FS_RESOLVED_BUF_LEN];
    shell_resolve_path(arg, path, sizeof(path));

    if (!shell_fs_acquire_and_mount(con)) {
        return;
    }
    exofs_file_t f;
    int rc = exofs_open(path, EXOFS_O_WRITE | EXOFS_O_CREATE, &f);
    if (rc == 0) {
        exofs_close(&f);
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

/* `cd [path]` -- no arg goes to "/"; "." is a validated no-op; ".." drops
 * the last '/'-separated component of shell_cwd; anything else resolves
 * via shell_resolve_path() as normal. Every case is validated with
 * exofs_opendir() before shell_cwd is actually updated, so an invalid
 * target leaves the cwd unchanged.
 *
 * Scope note: ".." only collapses as the *whole* argument here, not
 * embedded mid-path ("cd foo/../bar") -- ExoFS would still resolve that
 * correctly, since ".."/"." are real directory entries it walks
 * (docs/filesystem.md §7), but pwd would then show the uncollapsed literal
 * text. A full path canonicalizer is more than this toy shell needs. */
static void shell_run_cd(fb_console_t *con, const char *arg) {
    char target[SHELL_FS_RESOLVED_BUF_LEN];

    if (arg[0] == '\0') {
        target[0] = '/';
        target[1] = '\0';
    } else if (str_eq(arg, ".")) {
        size_t i = 0;
        while (shell_cwd[i] != '\0' && i < sizeof(target) - 1) {
            target[i] = shell_cwd[i];
            i++;
        }
        target[i] = '\0';
    } else if (str_eq(arg, "..")) {
        size_t len = 0;
        while (shell_cwd[len] != '\0') {
            len++;
        }
        size_t cut = 0;
        for (size_t k = 0; k < len; k++) {
            if (shell_cwd[k] == '/') {
                cut = k;
            }
        }
        if (cut == 0) {
            target[0] = '/';
            target[1] = '\0';
        } else {
            size_t i = 0;
            while (i < cut && i < sizeof(target) - 1) {
                target[i] = shell_cwd[i];
                i++;
            }
            target[i] = '\0';
        }
    } else {
        shell_resolve_path(arg, target, sizeof(target));
    }

    if (!shell_fs_acquire_and_mount(con)) {
        return;
    }
    exofs_dir_t dir;
    int rc = exofs_opendir(target, &dir);
    if (rc == 0) {
        exofs_closedir(&dir);
    }
    shell_fs_release();

    if (rc != 0) {
        fbcon_write(con, shell_fs_err_prefix);
        shell_write_i64(con, rc);
        fbcon_write(con, "\n");
        return;
    }

    size_t i = 0;
    while (target[i] != '\0' && i < sizeof(shell_cwd) - 1) {
        shell_cwd[i] = target[i];
        i++;
    }
    shell_cwd[i] = '\0';
    fbcon_write(con, shell_fs_ok_text);
}

static void shell_run_pwd(fb_console_t *con) {
    fbcon_write(con, shell_cwd);
    fbcon_write(con, "\n");
}

/* `echo <text...>` -- no filesystem interaction, just prints its argument
 * back, same as `about`. */
static void shell_run_echo(fb_console_t *con, const char *arg) {
    fbcon_write(con, arg);
    fbcon_write(con, "\n");
}

/* `disk` -- exo_diskstat() (#31) for presence/capacity, plus the two LBA
 * ranges this shell already knows about at compile time (no syscall needed
 * for those -- SHELL_FS_BASE_LBA/_VOL_SECTORS above, LIBOS_WAD_DISK_BASE_LBA
 * from src/doom_wad_disk.h), so "what disk is present" also answers "and
 * what's on it". */
static void shell_run_disk(fb_console_t *con) {
    exo_diskstat_t stat;
    if (exo_diskstat(&stat) != 0) {
        fbcon_write(con, shell_disk_fail_text);
        return;
    }
    if (!stat.present) {
        fbcon_write(con, shell_disk_none_text);
        return;
    }

    fbcon_write(con, "drive: present, ");
    shell_write_u32(con, stat.total_sectors);
    fbcon_write(con, " sectors (~");
    shell_write_u32(con, stat.total_sectors / 2048u); /* 512B/sector -> MiB */
    fbcon_write(con, " MiB)\n");
    fbcon_write(con, "  wad volume:   lba ");
    shell_write_u32(con, LIBOS_WAD_DISK_BASE_LBA);
    fbcon_write(con, "+\n");
    fbcon_write(con, "  shell volume: lba ");
    shell_write_u32(con, SHELL_FS_BASE_LBA);
    fbcon_write(con, "+ (");
    shell_write_u32(con, SHELL_FS_VOL_SECTORS);
    fbcon_write(con, " sectors)\n");
}

/* `dd read <lba> <count> <path>` / `dd write <path> <lba>` -- raw sector
 * I/O, bypassing ExoFS entirely for the disk side (this file's own top
 * comment on why it is the one deliberate exception to "never touch the
 * IWAD volume"). Just dispatches on the sub-verb; see
 * shell_run_dd_read()/_write() for the actual transfers. */
static void shell_run_dd(fb_console_t *con, const char *arg) {
    const char *rest;
    if (shell_cmd_match(arg, "read", &rest)) {
        shell_run_dd_read(con, rest);
    } else if (shell_cmd_match(arg, "write", &rest)) {
        shell_run_dd_write(con, rest);
    } else {
        fbcon_write(con, shell_dd_usage_text);
    }
}

/* `dd read <lba> <count> <path>` -- creates/truncates `path` and fills it
 * with `count` raw 512-byte sectors starting at `lba`, one
 * exo_disk_read()+exofs_write() pair per sector (no large buffer: the same
 * 512-byte-at-a-time shape shell_fs_copy_file() uses). One
 * shell_fs_acquire_and_mount() covers both halves -- the raw disk reads and
 * the ExoFS write are the same acquired disk. count == 0 is a deliberate
 * no-op, matching exo_disk_read's own convention. */
static void shell_run_dd_read(fb_console_t *con, const char *rest) {
    char tok[16];
    const char *p = rest;

    if (shell_next_token(&p, tok, sizeof(tok)) == 0) {
        fbcon_write(con, shell_dd_usage_text);
        return;
    }
    uint32_t lba = (uint32_t)atoi(tok);

    if (shell_next_token(&p, tok, sizeof(tok)) == 0) {
        fbcon_write(con, shell_dd_usage_text);
        return;
    }
    uint32_t count = (uint32_t)atoi(tok);

    if (*p == '\0') {
        fbcon_write(con, shell_dd_usage_text);
        return;
    }
    char path[SHELL_FS_RESOLVED_BUF_LEN];
    shell_resolve_path(p, path, sizeof(path));

    if (count == 0) {
        fbcon_write(con, shell_fs_ok_text);
        return;
    }

    if (!shell_fs_acquire_and_mount(con)) {
        return;
    }

    exofs_file_t f;
    int rc = exofs_open(path, EXOFS_O_WRITE | EXOFS_O_CREATE | EXOFS_O_TRUNC,
                        &f);
    if (rc != 0) {
        shell_fs_release();
        fbcon_write(con, shell_fs_err_prefix);
        shell_write_i64(con, rc);
        fbcon_write(con, "\n");
        return;
    }

    uint8_t sector[512];
    for (uint32_t i = 0; i < count; i++) {
        int64_t got = exo_disk_read(lba + i, sector, 1);
        if (got < 0) {
            rc = (int)got;
            break;
        }
        int64_t wrote = exofs_write(&f, sector, sizeof(sector));
        if (wrote < 0) {
            rc = (int)wrote;
            break;
        }
    }
    exofs_close(&f);
    shell_fs_release();

    if (rc != 0) {
        fbcon_write(con, shell_fs_err_prefix);
        shell_write_i64(con, rc);
        fbcon_write(con, "\n");
        return;
    }
    fbcon_write(con, shell_fs_ok_text);
}

/* `dd write <path> <lba>` -- the inverse: reads `path` back out through
 * ExoFS and writes it, 512 bytes at a time, to raw sectors starting at
 * `lba`. Sector count is derived from the file (ceil(size / 512)), not
 * passed in -- a final short read is zero-padded so the last sector is
 * still a full 512-byte write. */
static void shell_run_dd_write(fb_console_t *con, const char *rest) {
    char path_raw[SHELL_FS_PATH_BUF_LEN];
    const char *p = rest;

    if (shell_next_token(&p, path_raw, sizeof(path_raw)) == 0) {
        fbcon_write(con, shell_dd_usage_text);
        return;
    }
    char tok[16];
    if (shell_next_token(&p, tok, sizeof(tok)) == 0) {
        fbcon_write(con, shell_dd_usage_text);
        return;
    }
    uint32_t lba = (uint32_t)atoi(tok);

    char path[SHELL_FS_RESOLVED_BUF_LEN];
    shell_resolve_path(path_raw, path, sizeof(path));

    if (!shell_fs_acquire_and_mount(con)) {
        return;
    }

    exofs_file_t f;
    int rc = exofs_open(path, EXOFS_O_READ, &f);
    if (rc != 0) {
        shell_fs_release();
        fbcon_write(con, shell_fs_err_prefix);
        shell_write_i64(con, rc);
        fbcon_write(con, "\n");
        return;
    }

    uint8_t sector[512];
    uint32_t i = 0;
    for (;;) {
        int64_t got = exofs_read(&f, sector, sizeof(sector));
        if (got < 0) {
            rc = (int)got;
            break;
        }
        if (got == 0) {
            break;
        }
        if ((size_t)got < sizeof(sector)) {
            for (size_t z = (size_t)got; z < sizeof(sector); z++) {
                sector[z] = 0;
            }
        }
        int64_t wrote = exo_disk_write(lba + i, sector, 1);
        if (wrote < 0) {
            rc = (int)wrote;
            break;
        }
        i++;
    }
    exofs_close(&f);
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
