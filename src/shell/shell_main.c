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
 * which `mkfs` must never touch or `doom` breaks for the rest of the boot.
 * The one exception is `run` (SCRUM-230): it mounts the program volume at
 * LIBOS_WAD_DISK_BASE_LBA to read a package, and only stats/opens/reads
 * before unmounting -- every other command still mounts SHELL_FS_BASE_LBA.
 * `dd read` may read any LBA, the IWAD volume included; `dd write` is
 * confined to [0, SHELL_FS_BASE_LBA) so it can reach neither volume -- see
 * shell_run_dd_write()'s own comment.
 * Disk ownership (exo_disk_acquire/_release, #26/#27) is acquired, mounted,
 * used for exactly one filesystem call (or, for `dd`, one raw-sector loop
 * plus one filesystem call on the same acquired disk), and released again
 * by shell_fs_release() below -- never held across the idle loop -- so a
 * `wadview`/`clock`/`snake`/`doom` launch from this shell never contends
 * with a held-open volume.
 *
 * SCRUM-266: every path-taking command (`ls`, `cat`, `cd`, `mkdir`, `rm`,
 * `touch`, `cp`, `mv`) first routes its path through the mount table
 * (shell_route_arg(), src/shell/shell_mount.h) and so may land on a volume
 * other than the scratch one -- another disk's, a program slot's -- which it
 * then acquires and mounts the same way. `mkfs`, `run`, `programs`,
 * `install` and `dd` keep their fixed volumes.
 *
 * `mv` within one volume is exofs_rename() (SCRUM-240): files and
 * directories alike, nothing copied. Between volumes there is no entry to
 * re-link, so it is `cp`'s copy and then a delete. `cd`/`pwd` are the one
 * piece of state ExoFS deliberately has none of (docs/filesystem.md §7: "a
 * shell that wants a cd keeps the state itself"): shell_cwd below, resolved
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
#include "exo_admit.h"
#include "exo_grant.h"
#include "exo_syscall.h"
#include "fb.h"
#include "fb_console.h"
#include "libos_fb.h"
#include "libos_heap.h"
#include "libos_fs/exofs.h"
#include "exo_volume.h"
#include "ps2.h"
#include "shell/shell_complete.h"
#include "shell/shell_err.h"
#include "shell/shell_firstboot.h"
#include "shell/shell_help.h"
#include "shell/shell_hist.h"
#include "shell/shell_install.h"
#include "shell/shell_mount.h"
#include "stdlib.h"

#define SHELL_CMD_BUF_LEN 64

_Static_assert(SHELL_CMD_BUF_LEN == SHELL_HIST_LEN,
               "history entries must hold a whole command line");

/*
 * The shell's own scratch ExoFS volume -- deliberately NOT
 * LIBOS_WAD_DISK_BASE_LBA (src/doom_wad_disk.h), which holds freedoom2.wad
 * and must never be reformatted out from under `doom`. src/doom_wad_disk.h's
 * own comment documents [8192, 16383] as reserved, zeroed scratch space on
 * every build/exodoom_disk.img -- the same range
 * tests/kernel/test_exofs_k.c's EXOFS_TEST_BASE_LBA/VOL_SECTORS (8192/8192)
 * already uses under TESTING=1, unused here on a normal boot. If
 * src/doom_wad_disk.h's layout ever moves, this must move with it.
 *
 * There used to be a SHELL_FS_VOL_SECTORS = 8192u alongside this (how big a
 * volume `mkfs` lays down there) -- SCRUM-203 replaced it with
 * exofs_region_sectors(SHELL_FS_BASE_LBA, LIBOS_WAD_DISK_BASE_LBA, ...)
 * against the disk's real capacity (exo_diskstat, #31), so `mkfs` sizes the
 * volume from the drive that's actually attached instead of a constant that
 * silently assumed one. */
#define SHELL_FS_BASE_LBA    8192u

/* Block device every fixed LBA above lives on: the system disk, which the
 * kernel always numbers 0 (SCRUM-244). Both shell volumes, the program
 * volume and the `dd` window are on it. */
#define SHELL_SYS_DEV        0u

/* Block device `install` copies from: the install stick. Until USB mass
 * storage (SCRUM-247) that is the second IDE drive, which the kernel numbers
 * 1 (docs/drivers/ata.md); its ExoFS volume starts at LBA 0, as
 * docker/scripts/build.sh writes it. */
#define SHELL_SRC_DEV        1u
#define SHELL_SRC_BASE_LBA   0u

/* `install` copy buffer: large, because every chunk costs a mount of each
 * volume (shell_install.c), with a smaller fallback when the heap cannot
 * spare the large one. */
#define SHELL_INSTALL_CHUNK       (1024u * 1024u)
#define SHELL_INSTALL_CHUNK_SMALL (64u * 1024u)

/* `install <pattern>`: how many matches one command takes, and the room for
 * each one's path ("/" + an ExoFS name + NUL). The list is built before the
 * first copy; past the limit nothing is installed. */
#define SHELL_INSTALL_MAX_MATCHES 64u
#define SHELL_INSTALL_NAME_STRIDE (EXOFS_MAX_NAME + 2u)

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
static char shell_about_text[] = "ExoDoom shell LibOS\n";
static char shell_unknown_prefix[] = "unknown command: ";
static char shell_unknown_suffix[] = " (type 'help')\n";
static char shell_help_nomatch_text[] = "help: no commands match '";
/* Admission lines (SCRUM-232). Deliberate wording: "needs N pages, M free" is
 * the demo line -- the kernel reported, the launcher decided. */
static char shell_admit_refused_text[] = "run: refused: ";
static char shell_admit_needs_text[] = "needs ";
static char shell_admit_pages_text[] = " pages, ";
static char shell_admit_free_text[] = " free\n";
static char shell_admit_noctx_text[] = "no free context slot (all in use)\n";
static char shell_admit_degraded_text[] = "run: granted, degraded: ";
static char shell_admit_rec_text[] = "recommended ";
static char shell_admit_voices_text[] = "wants ";
static char shell_admit_have_text[] = " sound voices, ";
static char shell_admit_oom_text[] = "run: refused: out of pages, ";
static char shell_admit_oom_tail_text[] =
    " free (the IWAD is loaded on first use, after the check)\n";
static char shell_admit_fb_text[] = "run: framebuffer is held; taking it over\n";
static char shell_memstat_fail_text[] = "memstat: query failed\n";
static char shell_pslist_fail_text[] = "pslist: query failed\n";
static char shell_fs_ok_text[] = "ok\n";
/* First boot (SCRUM-234). */
static char shell_nosys_text[] = "no system volume";
static char shell_firstboot_prompt_text[] = "no system volume -- format? [y/N] ";
static char shell_firstboot_skip_text[] =
    "not formatted: program commands will fail until the system volume exists\n";
static char shell_firstboot_done_text[] =
    "system volume formatted; no programs installed\n";
static char shell_programs_none_text[] = "no programs installed\n";
static char shell_run_nosuch_text[] = "run: no such program: ";
static char shell_run_hint_text[] = "installed programs:\n";
static char shell_install_nodev_text[] = "install: no source disk\n";
static char shell_install_novol_text[] = "install: source disk has no volume\n";
static char shell_install_empty_text[] = "install: source disk is empty\n";
static char shell_install_copying_text[] = "installing ";
static char shell_install_done_text[] = "installed ";
static char shell_install_bytes_text[] = " bytes)\n";
static char shell_install_nomatch_text[] = "install: nothing on the install disk matches ";
static char shell_install_toomany_text[] = "install: too many matches; nothing installed\n";
static char shell_install_toplevel_text[] =
    "install: wildcards only match files at the top of the install disk\n";
static char shell_install_of_text[] = " of ";
static char shell_install_files_text[] = " files\n";
static char shell_fs_no_room_text[] = "mkfs: no room for shell volume on this disk\n";
static char shell_ls_dir_suffix[] = "/\n";
static char shell_ls_file_prefix[] = "  ";
static char shell_fs_toolong_text[] = "error: argument or path too long\n";
static char shell_fs_same_text[] = "error: source and destination are the same file\n";
static char shell_dd_badnum_text[] = "dd: not a number: ";
static char shell_dd_range_text[] =
    "dd: write refused -- only lbas below the shell volume are writable\n";
static char shell_disk_fail_text[] = "disk: query failed\n";
static char shell_disk_none_text[] = "disk 0: no system disk present\n";
/* mount / umount (SCRUM-266). */
static char shell_mount_none_text[] = "nothing mounted\n";
static char shell_mount_badname_text[] =
    "mount: expected diskN:volM, e.g. disk1:vol1 (see `disk`)\n";
static char shell_mount_novol_text[] = "mount: no such volume (see `disk`): ";
static char shell_mount_inuse_text[] =
    "mount: that volume belongs to a running program: ";
static char shell_mount_notempty_text[] = "mount: directory is not empty\n";
static char shell_mount_root_text[] = "mount: cannot mount over /\n";
static char shell_mount_self_text[] =
    "mount: that is the shell's own volume; it is already mounted at /\n";
static char shell_mount_already_text[] = "mount: something is already mounted there\n";
static char shell_umount_notmnt_text[] = "umount: not a mount point\n";
static char shell_umount_busy_text[] =
    "umount: busy (the current directory is inside it, or another mount is below it)\n";
static char shell_fs_mountpoint_text[] =
    "error: that is a mount point (umount it first)\n";
static char shell_cp_samedisk_text[] =
    "error: cannot copy between two volumes of the same disk\n";
static char shell_mv_exists_text[] = "error: destination already exists\n";
static char shell_mv_busy_text[] =
    "error: busy (the current directory or a mount point is inside it)\n";

/* ExoFS itself has no notion of a working directory by design
 * (docs/filesystem.md §7) -- this is that state, kept here instead. Always
 * absolute, always non-empty (shell_resolve_path() below assumes both).
 * Non-zero initializer so it lands in .data, not .bss, same reason every
 * other writable string in this file does. */
static char shell_cwd[SHELL_FS_RESOLVED_BUF_LEN] = "/";

/* Idle passes between looks at the fault log. The loop spins on a syscall
 * already (the keyboard poll), and almost every look finds nothing, so look
 * once per this many passes: a crash is still on screen within a frame. */
#define SHELL_FAULT_POLL_EVERY 64
static uint32_t shell_idle_passes;

static int str_eq(const char *a, const char *b);
static char shell_key_to_ascii(uint8_t key, uint8_t modifiers);
static void shell_backspace(fb_console_t *con);
static void shell_print_prompt(fb_console_t *con);
static void shell_run_command(fb_console_t *con, const char *line);
static void shell_write_u32(fb_console_t *con, uint32_t v);
static void shell_write_i64(fb_console_t *con, int64_t v);
static void shell_err_report(fb_console_t *con, shell_op_t op, int64_t err);
static int shell_fault_poll(exo_fault_info_t *rec);
static void shell_fault_print(fb_console_t *con, const exo_fault_info_t *rec);
static int shell_report_faults(fb_console_t *con);
static void shell_report_newest_fault(fb_console_t *con);
static void shell_run_memstat(fb_console_t *con, const char *arg);
static void shell_run_pslist(fb_console_t *con, const char *arg);
static int shell_cmd_match(const char *line, const char *cmd, const char **rest);
static int shell_fs_acquire_and_mount(fb_console_t *con);
static int shell_fs_acquire_and_mount_at(fb_console_t *con, uint32_t base_lba);
static void shell_run_run(fb_console_t *con, const char *arg);
static int shell_list_programs(fb_console_t *con, exofs_dir_t *dir);
static void shell_run_programs(fb_console_t *con, const char *arg);
static void shell_run_install(fb_console_t *con, const char *arg);
static void shell_firstboot_check(fb_console_t *con);
static int shell_probe_system_volume(void);
static int shell_format_volume(fb_console_t *con, uint32_t base_lba,
                               uint32_t region_end_lba);
static int shell_admit_report(fb_console_t *con, const exo_pkg_header_t *parsed);
static void shell_fs_release(void);
static int shell_resolve_path(const char *in, char *out, size_t out_len);
static int shell_resolve_arg(fb_console_t *con, const char *in, char *out,
                             size_t out_len);
static int shell_split_two(const char *rest, char *a, size_t alen,
                          char *b, size_t blen);
static int shell_next_token(const char **p, char *out, size_t out_len);
static int shell_parse_u32(const char *s, uint32_t *out);
static int shell_write_all(exofs_file_t *f, const void *buf, uint32_t n);
static int shell_fs_copy_file(exofs_vol_t *v, const char *src,
                              const char *dst);
static void shell_run_mkfs(fb_console_t *con, const char *arg);
static void shell_run_mkdir(fb_console_t *con, const char *arg);
static void shell_run_ls(fb_console_t *con, const char *arg);
static void shell_run_cat(fb_console_t *con, const char *arg);
static void shell_run_rm(fb_console_t *con, const char *arg);
static void shell_run_cp(fb_console_t *con, const char *arg);
static void shell_run_mv(fb_console_t *con, const char *arg);
static void shell_run_touch(fb_console_t *con, const char *arg);
static void shell_run_cd(fb_console_t *con, const char *arg);
static void shell_run_pwd(fb_console_t *con, const char *arg);
static void shell_run_echo(fb_console_t *con, const char *arg);
static void shell_run_disk(fb_console_t *con, const char *arg);
static void shell_run_mount(fb_console_t *con, const char *arg);
static void shell_run_umount(fb_console_t *con, const char *arg);
static void shell_run_dd(fb_console_t *con, const char *arg);
static void shell_run_dd_read(fb_console_t *con, const char *rest);
static int shell_dd_number(fb_console_t *con, const char **p, char *tok,
                           size_t tok_len, uint32_t *out);
static void shell_run_dd_write(fb_console_t *con, const char *rest);
static void shell_run_help(fb_console_t *con, const char *arg);
static void shell_run_clear(fb_console_t *con, const char *arg);
static void shell_run_about(fb_console_t *con, const char *arg);
static void shell_run_wadview(fb_console_t *con, const char *arg);
static void shell_run_clock(fb_console_t *con, const char *arg);
static void shell_run_top(fb_console_t *con, const char *arg);
static void shell_run_snake(fb_console_t *con, const char *arg);
static void shell_run_doom(fb_console_t *con, const char *arg);
static void shell_print_usage(fb_console_t *con, const char *name);
static int shell_tab(fb_console_t *con, char *cmd_buf, int *cmd_len,
                     shell_comp_set_t *set, int again);
static void shell_print_prompt(fb_console_t *con);

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
    shell_firstboot_check(&con);
    /* A shell the kernel relaunched because the last program crashed says so
     * (SCRUM-283); on a normal boot there is nothing to report. Only the
     * newest record: it is the one that explains this shell, and the older
     * ones were reported to the user by the shell it replaced. */
    shell_report_newest_fault(&con);
    shell_print_prompt(&con);

    static char cmd_buf[SHELL_CMD_BUF_LEN];
    int cmd_len = 0;
    static shell_hist_t hist;
    shell_hist_init(&hist);
    static shell_comp_set_t tab_set;
    int tab_again = 0;      /* tab_set is still the answer for this line */

    for (;;) {
        exo_kbd_event_t ev;

        while (exo_kbd_poll(&ev) == 1) {
            if (!ev.pressed) {
                continue;
            }

            /* Plain Tab completes (SCRUM-274). Ctrl+Tab is the context hotkey
             * and is consumed in src/ps2.c before it reaches the ring; the
             * modifier test is for a keyboard path that does not do that. */
            if (ev.key == KEY_TAB) {
                if (!(ev.modifiers & EXO_MOD_CTRL)) {
                    cmd_buf[cmd_len] = '\0';
                    tab_again = shell_tab(&con, cmd_buf, &cmd_len, &tab_set,
                                          tab_again);
                } else {
                    tab_again = 0;
                }
                continue;
            }
            tab_again = 0;

            if (ev.key == KEY_ENTER) {
                cmd_buf[cmd_len] = '\0';
                shell_hist_push(&hist, cmd_buf);
                fbcon_write(&con, "\n");
                shell_run_command(&con, cmd_buf);
                /* A launched program that crashed hands control back here;
                 * say what happened before the prompt returns (SCRUM-283). */
                (void)shell_report_faults(&con);
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

            if (ev.key == KEY_UP || ev.key == KEY_DOWN) {
                cmd_buf[cmd_len] = '\0';
                const char *line = (ev.key == KEY_UP)
                                       ? shell_hist_prev(&hist, cmd_buf)
                                       : shell_hist_next(&hist);
                if (line != NULL) {
                    /* Erase the row, then type the recalled line in its
                     * place; it is a plain edit buffer from here (SCRUM-273). */
                    while (cmd_len > 0) {
                        cmd_len--;
                        shell_backspace(&con);
                    }
                    while (line[cmd_len] != '\0' &&
                           cmd_len < SHELL_CMD_BUF_LEN - 1) {
                        cmd_buf[cmd_len] = line[cmd_len];
                        fbcon_putc(&con, line[cmd_len]);
                        cmd_len++;
                    }
                }
                continue;
            }

            char c = shell_key_to_ascii(ev.key, ev.modifiers);
            if (c != 0 && cmd_len < SHELL_CMD_BUF_LEN - 1) {
                cmd_buf[cmd_len++] = c;
                fbcon_putc(&con, c);
            }
        }

        /* A background program can crash while the shell is idle at its
         * prompt: report it on a fresh line, then put the prompt and the
         * half-typed command back. */
        exo_fault_info_t crashed;
        if ((++shell_idle_passes % SHELL_FAULT_POLL_EVERY) == 0 &&
            shell_fault_poll(&crashed)) {
            fbcon_write(&con, "\n");
            shell_fault_print(&con, &crashed);
            (void)shell_report_faults(&con);
            shell_print_prompt(&con);
            cmd_buf[cmd_len] = '\0';
            fbcon_write(&con, cmd_buf);
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
 * caller). Of the shifted symbols, only the three the shell's own syntax
 * uses are mapped (SCRUM-263): '?' (`<cmd> ?`), '*' (`help xxx*`) and '_'
 * (file names). The rest of the symbol row (!@#$...) is not -- not a
 * decoder limit, since src/ps2.c reports the unshifted key plus the
 * modifier mask, just nothing here needs it yet. */
static char shell_key_to_ascii(uint8_t key, uint8_t modifiers) {
    int shifted = (modifiers & EXO_MOD_SHIFT) != 0;

    if (key >= KEY_A && key <= KEY_Z) {
        char c = (char)('a' + (key - KEY_A));
        return shifted ? (char)(c - 32) : c;
    }
    if (shifted) {
        switch (key) {
        case KEY_SLASH: return '?';
        case KEY_8:     return '*';
        case KEY_MINUS: return '_';
        case KEY_SEMICOLON: return ':';    /* diskN:volM (SCRUM-266) */
        default:        break;
        }
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

/* One row per command: the help metadata `help`/`<cmd> ?`/usage errors read
 * (src/shell/shell_help.h), plus the handler and whether it accepts
 * arguments. This table is the only list of commands there is -- dispatch,
 * `help all` and every usage message come from it, so adding a command is
 * one row here (SCRUM-263). Keep it sorted by name: `help` lists rows in
 * table order and does not sort.
 *
 * takes_args == 0 means trailing text is a usage error rather than being
 * silently ignored. An argument of exactly "?" never reaches a handler --
 * shell_run_command() prints the usage text instead.
 *
 * const, so it lands in .rodata with the string literals it points at --
 * fine here, since unlike shell_cwd nothing ever writes it. */
typedef struct {
    shell_help_entry_t help;
    void (*run)(fb_console_t *con, const char *arg);
    int takes_args;
} shell_cmd_t;

static const shell_cmd_t shell_cmds[] = {
    { { "about", SHELL_GRP_SYSTEM, "Show what this shell is",
        "usage: about\n" },
      shell_run_about, 0 },
    { { "cat", SHELL_GRP_FS, "Print a file, or write text into it",
        "usage: cat <path>\n"
        "       cat <path> <text...>\n"
        "     <path>           Print the file\n"
        "     <path> <text...> Create or truncate the file; <text...> becomes its whole content\n" },
      shell_run_cat, 1 },
    { { "cd", SHELL_GRP_FS, "Change directory",
        "usage: cd [path]\n"
        "     (none)  Go to /\n"
        "     ..      Go up one directory\n"
        "     <path>  Absolute, or relative to the current directory\n" },
      shell_run_cd, 1 },
    { { "clear", SHELL_GRP_SYSTEM, "Clear the screen",
        "usage: clear\n" },
      shell_run_clear, 0 },
    { { "clock", SHELL_GRP_APPS, "Launch the clock (Ctrl+Tab returns to the shell)",
        "usage: clock\n"
        "     Same as `run clock.exo`\n" },
      shell_run_clock, 0 },
    { { "cp", SHELL_GRP_FS, "Copy a file",
        "usage: cp <src> <dst>\n"
        "     Creates or truncates <dst>; refuses when <src> and <dst> are the same file\n" },
      shell_run_cp, 1 },
    { { "dd", SHELL_GRP_DISK, "Raw sector copy between the disk and a file",
        "usage: dd read <lba> <count> <path>\n"
        "       dd write <path> <lba>\n"
        "     read   Copy <count> 512-byte sectors starting at <lba> into <path> (any LBA)\n"
        "     write  Copy <path> onto the disk at <lba>; only LBAs below the shell volume\n"
        "            are writable, so neither filesystem volume can be overwritten\n" },
      shell_run_dd, 1 },
    { { "disk", SHELL_GRP_DISK, "Show the drive's capacity and volume layout",
        "usage: disk\n" },
      shell_run_disk, 0 },
    { { "doom", SHELL_GRP_APPS, "Launch Doom (Ctrl+Tab returns to the shell)",
        "usage: doom\n"
        "     Running it again starts a second, independent instance; see pslist\n" },
      shell_run_doom, 0 },
    { { "echo", SHELL_GRP_SYSTEM, "Print text",
        "usage: echo <text...>\n" },
      shell_run_echo, 1 },
    { { "help", SHELL_GRP_SYSTEM, "Show help",
        "usage: help [all | fs | disk | apps | system | <command> | <prefix>[*]]\n"
        "     (none)     Show the help index\n"
        "     all        List every command\n"
        "     <group>    List one group's commands\n"
        "     <command>  Same as `<command> ?`\n"
        "     <prefix>*  List commands starting with <prefix>\n" },
      shell_run_help, 1 },
    { { "install", SHELL_GRP_APPS, "Copy a program from the install disk onto the system volume",
        "usage: install [name | pattern]\n"
        "     (none)     List the files on the install disk\n"
        "     <name>     Copy it to the system volume, replacing any copy there;\n"
        "                a name with no extension means <name>.exo, e.g. install doom\n"
        "     <pattern>  Copy every file at the top of the install disk that matches;\n"
        "                * matches any characters: install * (every .exo), install *.*\n" },
      shell_run_install, 1 },
    { { "ls", SHELL_GRP_FS, "List a directory",
        "usage: ls [path]\n"
        "     (none)  List the current directory\n" },
      shell_run_ls, 1 },
    { { "memstat", SHELL_GRP_SYSTEM, "Show physical page usage",
        "usage: memstat\n" },
      shell_run_memstat, 0 },
    { { "mkdir", SHELL_GRP_FS, "Create a directory",
        "usage: mkdir <path>\n" },
      shell_run_mkdir, 1 },
    { { "mkfs", SHELL_GRP_FS, "Format the shell's scratch volume (erases it)",
        "usage: mkfs\n"
        "     Destroys everything on the shell volume. Never touches the program/IWAD volume\n" },
      shell_run_mkfs, 0 },
    { { "mount", SHELL_GRP_DISK, "Mount a volume on an empty directory",
        "usage: mount [diskN:volM <dir>]\n"
        "     (none)        List what is mounted\n"
        "     diskN:volM    A volume `disk` lists: disk numbers start at 0, volume numbers at 1\n"
        "     <dir>         An existing, empty directory; the volume's files appear under it\n"
        "                   until `umount <dir>` or a reboot. Read/write.\n" },
      shell_run_mount, 1 },
    { { "mv", SHELL_GRP_FS, "Move or rename a file or directory",
        "usage: mv <src> <dst>\n"
        "     Same volume: renames in place, nothing is copied. Replaces an\n"
        "     existing file <dst>; refuses an existing directory. The\n"
        "     replace is not atomic: if the rename then fails (disk full,\n"
        "     device error), <dst> is already gone and <src> stays put.\n"
        "     Between volumes: copies <src> to <dst>, then deletes <src>\n"
        "     (files only)\n" },
      shell_run_mv, 1 },
    { { "programs", SHELL_GRP_APPS, "List installed programs",
        "usage: programs\n"
        "     Lists the .exo packages on the system volume\n" },
      shell_run_programs, 0 },
    { { "pslist", SHELL_GRP_SYSTEM, "List live contexts",
        "usage: pslist\n" },
      shell_run_pslist, 0 },
    { { "pwd", SHELL_GRP_FS, "Print the current directory",
        "usage: pwd\n" },
      shell_run_pwd, 0 },
    { { "rm", SHELL_GRP_FS, "Remove a file or an empty directory",
        "usage: rm <path>\n" },
      shell_run_rm, 1 },
    { { "run", SHELL_GRP_APPS, "Launch a program package",
        "usage: run <package>\n"
        "     <package>  A .exo file on the program volume, e.g. run snake.exo\n"
        "                The .exo may be left off and case is ignored: run snake,\n"
        "                run SNAKE. A file named exactly as typed wins.\n" },
      shell_run_run, 1 },
    { { "snake", SHELL_GRP_APPS, "Launch Snake",
        "usage: snake\n"
        "     Same as `run snake.exo`\n" },
      shell_run_snake, 0 },
    { { "top", SHELL_GRP_APPS, "Live resource monitor (Q/Esc exits, Ctrl+Tab returns to the shell)",
        "usage: top\n"
        "     Same as `run top.exo`; pages, contexts, bindings and sound voices\n" },
      shell_run_top, 0 },
    { { "touch", SHELL_GRP_FS, "Create an empty file if it does not exist",
        "usage: touch <path>\n" },
      shell_run_touch, 1 },
    { { "umount", SHELL_GRP_DISK, "Unmount a volume mounted with `mount`",
        "usage: umount <dir>\n"
        "     Refused while the current directory is inside <dir> or another mount is below it\n" },
      shell_run_umount, 1 },
    { { "wadview", SHELL_GRP_APPS, "Launch the WAD/automap viewer (Q or Esc exits)",
        "usage: wadview\n" },
      shell_run_wadview, 0 },
};

#define SHELL_CMD_COUNT (sizeof(shell_cmds) / sizeof(shell_cmds[0]))

static const shell_cmd_t *shell_cmd_find(const char *tok, size_t len) {
    for (size_t i = 0; i < SHELL_CMD_COUNT; i++) {
        if (shell_ci_token_eq(tok, len, shell_cmds[i].help.name)) {
            return &shell_cmds[i];
        }
    }
    return 0;
}

/* The same text as `<name> ?` -- every usage error goes through here so the
 * two can't disagree. */
static void shell_print_usage(fb_console_t *con, const char *name) {
    size_t len = 0;
    while (name[len] != '\0') {
        len++;
    }
    const shell_cmd_t *cmd = shell_cmd_find(name, len);
    if (cmd != 0) {
        fbcon_write(con, cmd->help.usage);
    }
}

static void shell_run_command(fb_console_t *con, const char *line) {
    while (*line == ' ') {
        line++;
    }
    if (line[0] == '\0') {
        return;
    }

    size_t len = 0;
    while (line[len] != '\0' && line[len] != ' ') {
        len++;
    }
    const char *rest = line + len;
    while (*rest == ' ') {
        rest++;
    }

    const shell_cmd_t *cmd = shell_cmd_find(line, len);
    if (cmd == 0) {
        fbcon_write(con, shell_unknown_prefix);
        for (size_t i = 0; i < len; i++) {
            fbcon_putc(con, line[i]);
        }
        fbcon_write(con, shell_unknown_suffix);
        return;
    }
    if (str_eq(rest, "?") || (!cmd->takes_args && rest[0] != '\0')) {
        fbcon_write(con, cmd->help.usage);
        return;
    }
    cmd->run(con, rest);
}

static void shell_help_list(fb_console_t *con, const shell_help_query_t *q) {
    char row[128];
    int any = 0;
    for (size_t i = 0; i < SHELL_CMD_COUNT; i++) {
        if (shell_help_entry_matches(&shell_cmds[i].help, q)) {
            shell_help_format_row(&shell_cmds[i].help, row, sizeof(row));
            fbcon_write(con, row);
            any = 1;
        }
    }
    if (!any) {
        fbcon_write(con, shell_help_nomatch_text);
        for (size_t i = 0; i < q->word_len; i++) {
            fbcon_putc(con, q->word[i]);
        }
        if (q->kind == SHELL_HELP_Q_PREFIX) {
            fbcon_putc(con, '*');
        }
        fbcon_write(con, "'\n");
    }
}

/* `help [all | <group> | <command> | <prefix>[*]]`. A group name wins over a
 * command of the same name -- `help disk` lists the DISK group, which
 * includes `disk` itself; `disk ?` still shows that one command. */
static void shell_run_help(fb_console_t *con, const char *arg) {
    shell_help_query_t q = shell_help_query_parse(arg);

    if (q.kind == SHELL_HELP_Q_INDEX) {
        fbcon_write(con,
            "HELP - Type a command followed by a space and '?' for its options (e.g. ls ?)\n"
            "HELP ALL      List every command\n"
            "HELP FS       Filesystem commands (mkfs, ls, cat, ...)\n"
            "HELP DISK     Raw disk commands\n"
            "HELP APPS     Programs you can launch\n"
            "HELP SYSTEM   Shell and system information\n"
            "HELP xxx*     Commands starting with xxx\n");
        return;
    }
    if (q.kind == SHELL_HELP_Q_WORD) {
        const shell_cmd_t *cmd = shell_cmd_find(q.word, q.word_len);
        if (cmd != 0) {
            fbcon_write(con, cmd->help.usage);
            return;
        }
    }
    shell_help_list(con, &q);
}

static void shell_run_clear(fb_console_t *con, const char *arg) {
    (void)arg;
    fbcon_clear(con);
}

static void shell_run_about(fb_console_t *con, const char *arg) {
    (void)arg;
    fbcon_write(con, shell_about_text);
}

static void shell_run_wadview(fb_console_t *con, const char *arg) {
    (void)arg;
    /* Like clock/snake/doom: `run wadview.exo` (SCRUM-232). The package asks
     * for the IWAD in its manifest (EXO_PKG_WANT_WAD), so it launches through
     * #36 and the same admission check. Does not return until the viewer
     * yields back. */
    shell_run_run(con, "wadview.exo");
}

static void shell_run_clock(fb_console_t *con, const char *arg) {
    (void)arg;
    /* Like snake below, the clock is no longer in the kernel image
     * (SCRUM-231): this is `run clock.exo`. It never yields on its own
     * (src/libos_clock/libos_clock.c's own comment on why), so control
     * comes back via Ctrl+Tab (SCRUM-111). */
    shell_run_run(con, "clock.exo");
}

static void shell_run_top(fb_console_t *con, const char *arg) {
    (void)arg;
    /* `run top.exo` (SCRUM-236). Like the clock it never yields on its
     * own, so control comes back via Ctrl+Tab. */
    shell_run_run(con, "top.exo");
}

static void shell_run_snake(fb_console_t *con, const char *arg) {
    (void)arg;
    /* Same round-trip convention as "wadview" above, but Snake is no
     * longer in the kernel image (SCRUM-230): this is `run snake.exo`,
     * read off the program volume. Does not return until Snake exits
     * (src/libos_snake/libos_snake.c's own EXO_SYS_EXIT + exo_yield()
     * fallback) and control round-robins back here. */
    shell_run_run(con, "snake.exo");
}

static void shell_run_doom(fb_console_t *con, const char *arg) {
    (void)arg;
    /* `run doom.exo` (SCRUM-232): the same admission check as every other
     * program, so a machine without the pages says "needs N pages, M free"
     * instead of failing inside the loader. Doom's loop does not exit on its
     * own -- D_DoomLoop() runs forever and I_Error halts -- so in practice
     * this does not come back; Ctrl+Tab (SCRUM-111) is how you reach the
     * shell again.
     *
     * Running "doom" again from a different foreground context launches a
     * second, independent instance rather than replacing this one
     * (SCRUM-196): #36 only reclaims context rows that have actually exited,
     * not ones merely switched away from. `pslist` lists every live context
     * id. There is no split-screen compositor yet (SCRUM-197), so only the
     * foreground instance is visible. */
    shell_run_run(con, "doom.exo");
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
 * shell_err_report().
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

/* Print "<lead>: <sentence>" for a failed `op` (SCRUM-271). The numeric code
 * follows only when shell_err_text() had no sentence of its own for it, so
 * an unmapped failure stays greppable. */
static void shell_err_report(fb_console_t *con, shell_op_t op, int64_t err) {
    int mapped = 0;
    const char *lead = shell_err_lead(op);
    const char *text = shell_err_text(op, err, &mapped);
    if (lead != NULL) {
        fbcon_write(con, lead);
        fbcon_write(con, ": ");
    }
    fbcon_write(con, text);
    if (!mapped) {
        fbcon_write(con, " (err=");
        shell_write_i64(con, err);
        fbcon_write(con, ")");
    }
    fbcon_write(con, "\n");
}

/* The newest fault record this shell has already told the user about
 * (SCRUM-283). exo_fault_info (#41) hands out records in seq order, so asking
 * for "newer than this" reports each crash once, whichever program died and
 * wherever the shell was when it happened. A relaunched shell starts at 0 and
 * so reports the crash that replaced its predecessor. */
static uint64_t shell_fault_seen;

/* Fetch the next unreported fault into *rec and mark it seen. 1 if there was
 * one, 0 if not (-EXO_ENOENT is the normal answer). */
static int shell_fault_poll(exo_fault_info_t *rec) {
    if (exo_fault_info(shell_fault_seen, rec) != 0) {
        return 0;
    }
    shell_fault_seen = rec->seq;
    return 1;
}

/* "program X stopped: <why>" on its own line, in the warning colour. */
static void shell_fault_print(fb_console_t *con, const exo_fault_info_t *rec) {
    char line[80];
    shell_fault_format(line, sizeof(line), rec->name, rec->reason);
    fbcon_set_color(con, 255, 120, 120, 0, 0, 0);
    fbcon_write(con, line);
    fbcon_write(con, "\n");
    fbcon_set_color(con, 220, 220, 220, 0, 0, 0);
}

/* First thing a shell does: say what ended the program before it, if the
 * kernel relaunched this shell after one. Takes only the newest record and
 * marks everything up to it seen, so records an earlier shell already
 * reported are not replayed. */
static void shell_report_newest_fault(fb_console_t *con) {
    exo_fault_info_t rec;
    if (exo_fault_info(EXO_FAULT_NEWEST, &rec) != 0) {
        return;
    }
    shell_fault_seen = rec.seq;
    shell_fault_print(con, &rec);
}

/* Report every fault not yet reported. Returns how many. */
static int shell_report_faults(fb_console_t *con) {
    exo_fault_info_t rec;
    int n = 0;
    while (shell_fault_poll(&rec)) {
        shell_fault_print(con, &rec);
        n++;
    }
    return n;
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
 * show an uncollapsed ".." component (shell_run_cd()'s own comment).
 *
 * Returns 0, or -EXO_EINVAL if the result does not fit in `out`. A truncated
 * path is a *different* path -- `rm`/`mv`/`cp` would act on whatever the
 * cut-off prefix happens to name -- so callers refuse rather than proceed;
 * shell_resolve_arg() below is that check plus its diagnostic. */
static int shell_resolve_path(const char *in, char *out, size_t out_len) {
    if (in[0] == '/') {
        size_t i = 0;
        while (in[i] != '\0' && i < out_len - 1) {
            out[i] = in[i];
            i++;
        }
        out[i] = '\0';
        return in[i] == '\0' ? 0 : -EXO_EINVAL;
    }

    size_t i = 0;
    while (shell_cwd[i] != '\0' && i < out_len - 1) {
        out[i] = shell_cwd[i];
        i++;
    }
    if (i > 0 && out[i - 1] != '/' && i < out_len - 1) {
        out[i++] = '/';
    }
    if (shell_cwd[i] != '\0') {
        out[i] = '\0';
        return -EXO_EINVAL;
    }
    size_t j = 0;
    while (in[j] != '\0' && i < out_len - 1) {
        out[i++] = in[j++];
    }
    out[i] = '\0';
    return in[j] == '\0' ? 0 : -EXO_EINVAL;
}

/* shell_resolve_path(), printing the diagnostic itself on failure. Returns 1
 * if `out` holds the full resolved path, 0 (already reported) if not. */
static int shell_resolve_arg(fb_console_t *con, const char *in, char *out,
                             size_t out_len) {
    if (shell_resolve_path(in, out, out_len) != 0) {
        fbcon_write(con, shell_fs_toolong_text);
        return 0;
    }
    return 1;
}

/* Extract the next whitespace-delimited token from *p into `out`, then
 * advance *p past it and any following spaces. Returns the token length, 0
 * if *p was already empty/all spaces (the caller's "missing argument"
 * case), or -1 if the token does not fit in `out`. An over-long token is
 * refused rather than split: splitting it would hand its tail to the
 * caller as the *next* argument, so `cp <long-name> dst` would take part of
 * the first name as its destination. Shared by `cat`, `cp`/`mv`'s two-path
 * split and `dd`'s numeric/path arguments. */
static int shell_next_token(const char **p, char *out, size_t out_len) {
    const char *s = *p;
    size_t i = 0;
    while (s[i] != '\0' && s[i] != ' ') {
        if (i >= out_len - 1) {
            out[0] = '\0';
            return -1;
        }
        out[i] = s[i];
        i++;
    }
    out[i] = '\0';
    s += i;
    while (*s == ' ') {
        s++;
    }
    *p = s;
    return (int)i;
}

/* 1 with both tokens filled, 0 if either is missing, -1 if either is too
 * long (shell_next_token()). */
static int shell_split_two(const char *rest, char *a, size_t alen,
                          char *b, size_t blen) {
    const char *p = rest;
    int n = shell_next_token(&p, a, alen);
    if (n <= 0) {
        return n;
    }
    n = shell_next_token(&p, b, blen);
    if (n <= 0) {
        return n;
    }
    return 1;
}

/* Decimal digits only, no sign, no overflow past UINT32_MAX. atoi() is not
 * fit for an LBA: "-1" would cast to 0xFFFFFFFF and "12abc" to 12. Returns
 * 1 with *out set, or 0. */
static int shell_parse_u32(const char *s, uint32_t *out) {
    uint32_t v = 0;
    if (*s == '\0') {
        return 0;
    }
    for (; *s != '\0'; s++) {
        if (*s < '0' || *s > '9') {
            return 0;
        }
        uint32_t d = (uint32_t)(*s - '0');
        if (v > (0xFFFFFFFFu - d) / 10u) {
            return 0;
        }
        v = v * 10u + d;
    }
    *out = v;
    return 1;
}

/* exofs_write() all of `buf`, or fail. exofs_write() can return fewer bytes
 * than asked when the volume fills; treating that as success is how `mv`
 * would unlink a source whose copy was cut short. Returns 0, the negative
 * error exofs_write() reported, or -EXO_ENOSPC for a short write. */
static int shell_write_all(exofs_file_t *f, const void *buf, uint32_t n) {
    int64_t wrote = exofs_write(f, buf, n);
    if (wrote < 0) {
        return (int)wrote;
    }
    if ((uint64_t)wrote != n) {
        return -EXO_ENOSPC;
    }
    return 0;
}

/* Shared by `cp` and `mv` (mv = this, then exofs_unlink(src) on success).
 * Streams through a 512-byte stack buffer rather than one allocation sized
 * to the whole file -- same shape `cat`'s read loop already uses, works for
 * any file size. Returns 0, -EXO_EEXIST if `src` and `dst` are the same
 * file, -EXO_ENOSPC if the volume filled mid-copy, or the first negative
 * error either open or any read/write step reports. A non-zero return is
 * what keeps `mv` from unlinking its source.
 *
 * `dst` is opened WITHOUT EXOFS_O_TRUNC first, so the same-file check runs
 * before anything is destroyed: `cp a a` would otherwise truncate the source
 * it is about to read, and `mv a a` would then unlink the (now empty) only
 * copy. The check compares directory-entry refs, not path text, so aliases
 * like "a", "./a" and "/dir/../a" are all caught. The truncating reopen
 * only happens once the two are known to differ. */
static int shell_fs_copy_file(exofs_vol_t *v, const char *src,
                              const char *dst) {
    exofs_file_t fsrc, fdst;

    int rc = exofs_open(v, src, EXOFS_O_READ, &fsrc);
    if (rc != 0) {
        return rc;
    }
    rc = exofs_open(v, dst, EXOFS_O_WRITE | EXOFS_O_CREATE, &fdst);
    if (rc != 0) {
        exofs_close(&fsrc);
        return rc;
    }
    int same = fdst.ent_block == fsrc.ent_block &&
               fdst.ent_index == fsrc.ent_index;
    exofs_close(&fdst);
    if (same) {
        exofs_close(&fsrc);
        return -EXO_EEXIST;
    }
    rc = exofs_open(v, dst, EXOFS_O_WRITE | EXOFS_O_TRUNC, &fdst);
    if (rc != 0) {
        exofs_close(&fsrc);
        return rc;
    }

    uint8_t buf[512];
    int64_t got;
    while ((got = exofs_read(&fsrc, buf, sizeof(buf))) > 0) {
        rc = shell_write_all(&fdst, buf, (uint32_t)got);
        if (rc != 0) {
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
/* The shell binds only the part of the disk it is about to touch (SCRUM-237),
 * never the whole device, so a program that owns a volume of its own does not
 * lock every shell filesystem command out with -EXO_EBUSY. The scratch volume
 * and the raw-sector `dd` window both sit below the program volume; the
 * program volume runs up to where the per-program slots start. */
static int64_t shell_disk_acquire_for(uint32_t base_lba) {
    if (base_lba >= LIBOS_WAD_DISK_BASE_LBA) {
        return exo_disk_acquire(SHELL_SYS_DEV, LIBOS_WAD_DISK_BASE_LBA,
                                EXO_VOLUME_REGION_BASE_LBA -
                                    LIBOS_WAD_DISK_BASE_LBA);
    }
    return exo_disk_acquire(SHELL_SYS_DEV, 0, LIBOS_WAD_DISK_BASE_LBA);
}

/* The volume refs of the two ExoFS volumes this shell has always known at
 * compile time, with the disk extent shell_disk_acquire_for() binds for
 * each. */
static shell_volref_t shell_sys_volref(uint32_t base_lba) {
    shell_volref_t v = { SHELL_SYS_DEV, base_lba, 0, 0, 0 };
    if (base_lba >= LIBOS_WAD_DISK_BASE_LBA) {
        v.extent_base = LIBOS_WAD_DISK_BASE_LBA;
        v.extent_count = EXO_VOLUME_REGION_BASE_LBA - LIBOS_WAD_DISK_BASE_LBA;
        v.volno = 2;
    } else {
        v.extent_count = LIBOS_WAD_DISK_BASE_LBA;
        v.volno = 1;
    }
    return v;
}

/* The device shell_fs_acquire_and_mount_vol() last bound, so that
 * shell_fs_release() gives back the right one: a mounted volume can be on
 * any disk now, not only the system disk. */
static uint32_t shell_fs_held_dev;

/* The volume shell_fs_acquire_and_mount_vol() last mounted: what a command's
 * exofs_* calls run against until shell_fs_release() unmounts it. Several
 * volumes can now be mounted together (SCRUM-265) -- `cp`/`mv` across disks
 * mounts two for one copy -- but the common command still works on exactly
 * one, so it is kept here rather than threaded through every helper. */
static exofs_vol_t *shell_cur_vol;

/* Acquire the extent `v` lives in and mount it. Prints its own diagnostic
 * and returns 0 on failure so each caller can just return. */
static int shell_fs_acquire_and_mount_vol(fb_console_t *con,
                                          const shell_volref_t *v) {
    int64_t rc = exo_disk_acquire(v->dev, v->extent_base, v->extent_count);
    if (rc != 0) {
        shell_err_report(con, SHELL_OP_DISK, rc);
        return 0;
    }

    int mrc = exofs_mount(v->dev, v->base, &shell_cur_vol);
    if (mrc != 0) {
        shell_err_report(con, SHELL_OP_MOUNT, mrc);
        exo_disk_release(v->dev);
        return 0;
    }

    shell_fs_held_dev = v->dev;
    return 1;
}

static int shell_fs_acquire_and_mount(fb_console_t *con) {
    return shell_fs_acquire_and_mount_at(con, SHELL_FS_BASE_LBA);
}

/* Same, for a volume other than the shell's scratch one -- `run` reads
 * program packages off the program volume at LIBOS_WAD_DISK_BASE_LBA. */
static int shell_fs_acquire_and_mount_at(fb_console_t *con, uint32_t base_lba) {
    shell_volref_t v = shell_sys_volref(base_lba);
    return shell_fs_acquire_and_mount_vol(con, &v);
}

static void shell_fs_release(void) {
    exofs_unmount(shell_cur_vol);
    shell_cur_vol = NULL;
    exo_disk_release(shell_fs_held_dev);
    shell_fs_held_dev = SHELL_SYS_DEV;
}

/* ---- Volumes and mounts (SCRUM-266) --------------------------------------
 *
 * `disk` lists the volumes the shell can find, `mount` attaches one to a
 * directory, and every path-taking filesystem command routes through
 * shell_route_arg() to learn which volume its path is on. See
 * src/shell/shell_mount.h for the table and docs/filesystem.md "Mounts" for
 * how a command mounts the volumes it routes to (SCRUM-265: several at once).
 */

/* Where a volume might be. The system disk has a fixed layout
 * (src/exo_volume.h): the shell's scratch volume, the program volume, then
 * the per-program slots. Every other disk is tried at LBA 0 only, which is
 * where the install disk's volume is. */
#define SHELL_VOL_MAX (2u + EXO_VOLUME_MAX_SLOTS)

typedef enum {
    SHELL_VOLK_SHELL,
    SHELL_VOLK_SYSTEM,
    SHELL_VOLK_SLOT,
    SHELL_VOLK_DISK
} shell_volkind_t;

typedef struct {
    shell_volref_t ref;
    shell_volkind_t kind;
    uint32_t slot;       /* SHELL_VOLK_SLOT only */
    int busy;            /* held by a running program: cannot be mounted */
} shell_volinfo_t;

static uint32_t shell_vol_candidates(uint32_t dev) {
    return dev == SHELL_SYS_DEV ? SHELL_VOL_MAX : 1u;
}

static void shell_vol_candidate(uint32_t dev, uint32_t i, shell_volinfo_t *out) {
    out->busy = 0;
    out->slot = 0;
    if (dev != SHELL_SYS_DEV) {
        shell_volref_t v = { dev, 0, 0, 0, 0 };
        out->ref = v;
        out->kind = SHELL_VOLK_DISK;
    } else if (i == 0) {
        out->ref = shell_sys_volref(SHELL_FS_BASE_LBA);
        out->kind = SHELL_VOLK_SHELL;
    } else if (i == 1) {
        out->ref = shell_sys_volref(LIBOS_WAD_DISK_BASE_LBA);
        out->kind = SHELL_VOLK_SYSTEM;
    } else {
        uint32_t base = exo_volume_slot_base(i - 2u);
        shell_volref_t v = { dev, base, base, EXO_VOLUME_SLOT_SECTORS, 0 };
        out->ref = v;
        out->kind = SHELL_VOLK_SLOT;
        out->slot = i - 2u;
    }
}

/* Whether an ExoFS superblock sits at the volume's base: 1 yes, 0 no (blank,
 * foreign, unreadable, or past the end of the disk), or -EXO_EBUSY when the
 * extent belongs to a running program and so could not be looked at. Reads
 * one sector rather than mounting, which would read the whole FAT. Leaves no
 * binding held. */
static int shell_vol_probe(const shell_volref_t *r) {
    int64_t rc = exo_disk_acquire(r->dev, r->extent_base, r->extent_count);
    if (rc == -EXO_EBUSY) {
        return -EXO_EBUSY;
    }
    if (rc != 0) {
        return 0;
    }
    int found = 0;
    uint8_t *sec = libos_heap_alloc(EXOFS_BLOCK_SIZE);
    if (sec != NULL && exo_disk_read(r->dev, r->base, sec, 1) == 1) {
        found = shell_mount_sb_valid(sec);
    }
    libos_heap_free(sec);
    exo_disk_release(r->dev);
    return found;
}

/* The volumes found on `dev`. A volume's number is its candidate's position
 * (1-based) -- on the system disk 1 is the shell volume, 2 the program
 * volume, 3 and up the slots -- so it does not change when another volume is
 * formatted (`mkfs`) or a program holding a slot starts or exits. A
 * candidate with no superblock is simply absent from the list; one held by a
 * running program is listed as busy. Returns how many were found; the array
 * is in number order but has gaps in the numbering. */
static uint32_t shell_vol_list(uint32_t dev, shell_volinfo_t *out, uint32_t max) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < shell_vol_candidates(dev) && n < max; i++) {
        shell_vol_candidate(dev, i, &out[n]);
        int p = shell_vol_probe(&out[n].ref);
        if (p == 0) {
            continue;
        }
        out[n].busy = (p < 0);
        out[n].ref.volno = i + 1u;
        n++;
    }
    return n;
}

static void shell_vol_describe(fb_console_t *con, const shell_volinfo_t *v) {
    switch (v->kind) {
    case SHELL_VOLK_SHELL:  fbcon_write(con, "shell volume"); break;
    case SHELL_VOLK_SYSTEM: fbcon_write(con, "system volume"); break;
    case SHELL_VOLK_SLOT:
        fbcon_write(con, "program slot ");
        shell_write_u32(con, v->slot);
        break;
    default:
        fbcon_write(con, v->ref.dev == SHELL_SRC_DEV ? "install disk volume"
                                                     : "volume");
        break;
    }
}

/* Where a path argument lands: the absolute, normalized path, the volume it
 * is on, and the path within that volume. mount_idx >= 0 with inner "/" means
 * the path is a mount point itself. */
typedef struct {
    char abs[SHELL_FS_RESOLVED_BUF_LEN];
    char inner[SHELL_FS_RESOLVED_BUF_LEN];
    shell_volref_t vol;
    int mount_idx;
} shell_route_t;

static shell_mount_table_t shell_mounts;

/* Route the already-absolute `abs`. Prints the diagnostic itself and returns
 * 0 on failure (a path that no longer fits once normalized). */
static int shell_route_abs(fb_console_t *con, const char *abs, shell_route_t *r) {
    shell_volref_t root = shell_sys_volref(SHELL_FS_BASE_LBA);
    if (shell_mount_normalize(abs, r->abs, sizeof(r->abs)) != 0 ||
        shell_mount_route(&shell_mounts, &root, r->abs, &r->vol, r->inner,
                          sizeof(r->inner), &r->mount_idx) != 0) {
        fbcon_write(con, shell_fs_toolong_text);
        return 0;
    }
    return 1;
}

/* shell_resolve_arg() followed by shell_route_abs(): the cwd-relative `in`
 * becomes (volume, path within it). */
static int shell_route_arg(fb_console_t *con, const char *in, shell_route_t *r) {
    char abs[SHELL_FS_RESOLVED_BUF_LEN];
    if (!shell_resolve_arg(con, in, abs, sizeof(abs))) {
        return 0;
    }
    return shell_route_abs(con, abs, r);
}

static int shell_same_vol(const shell_volref_t *a, const shell_volref_t *b) {
    return a->dev == b->dev && a->base == b->base;
}

/* Admission check for `run` (SCRUM-232): compare the package's manifest with
 * what the kernel reports free (exo_resstat, #38) and say what was decided.
 * `parsed` is the package's header as run's single exo_pkg_parse() call
 * produced it, or NULL if it did not parse. Returns 1 to go ahead (granted or
 * degraded), 0 if refused. A package that did not parse is let through: exo_launch_pkg() owns the format check and
 * reports it with its own error, and this must not second-guess it. The
 * report is a snapshot, so #36 keeps a page-count backstop of its own. */
static int shell_admit_report(fb_console_t *con, const exo_pkg_header_t *parsed) {
    if (parsed == NULL) {
        return 1;
    }
    const exo_pkg_header_t hdr = *parsed;
    exo_resstat_t res;
    if (exo_resstat(&res) != 0) {
        return 1;
    }

    exo_admit_t a;
    exo_admit(&hdr, &res, &a);

    if (a.verdict == EXO_ADMIT_REFUSED) {
        fbcon_write(con, shell_admit_refused_text);
        if (a.reason == EXO_ADMIT_CONTEXTS) {
            fbcon_write(con, shell_admit_noctx_text);
        } else {
            fbcon_write(con, shell_admit_needs_text);
            shell_write_u32(con, a.needed);
            fbcon_write(con, shell_admit_pages_text);
            shell_write_u32(con, a.have);
            fbcon_write(con, shell_admit_free_text);
        }
        return 0;
    }
    if (a.verdict == EXO_ADMIT_DEGRADED) {
        fbcon_write(con, shell_admit_degraded_text);
        if (a.reason == EXO_ADMIT_PAGES) {
            fbcon_write(con, shell_admit_rec_text);
            shell_write_u32(con, a.needed);
            fbcon_write(con, shell_admit_pages_text);
            shell_write_u32(con, a.have);
            fbcon_write(con, shell_admit_free_text);
        } else {
            fbcon_write(con, shell_admit_voices_text);
            shell_write_u32(con, a.needed);
            fbcon_write(con, shell_admit_have_text);
            shell_write_u32(con, a.have);
            fbcon_write(con, shell_admit_free_text);
        }
    }
    if (a.fb_displaces) {
        fbcon_write(con, shell_admit_fb_text);
    }
    return 1;
}

/* Launch a package and log its resource grant first (SCRUM-235): build the
 * context without entering it, read what it holds (exo_grantstat, #39), write
 * that next to what the manifest asked for to COM1, then enter it (#40).
 * Returns what exo_launch_pkg()/exo_launch_start() would. Serial only: the
 * program takes the screen the moment it runs, so an on-screen copy would
 * either vanish unread or need the shell to hold the launch back. */
static int64_t shell_launch_with_grant(const void *pkg, uint32_t len,
                                       const exo_pkg_header_t *hdr,
                                       const char *prog_name) {
    if (hdr == NULL) {
        /* Not ours to judge; the kernel reports the format error. */
        return exo_launch_pkg(pkg, len, 0, prog_name);
    }

    int64_t ctx = exo_launch_pkg(pkg, len, EXO_LAUNCH_HOLD, prog_name);
    if (ctx <= 0) {
        return ctx < 0 ? ctx : -EXO_EINVAL;
    }

    exo_grant_t g;
    exo_resstat_t res;
    if (exo_grantstat((uint64_t)ctx, &g) == 0 && exo_resstat(&res) == 0) {
        char text[EXO_GRANT_TEXT_MAX];
        size_t n = exo_grant_format(text, sizeof(text), hdr, &g, &res,
                                    g.fb_owner);
        exo_serial_write(text, n);
    } else {
        static const char unavailable[] = "run: grant read-out unavailable\n";
        exo_serial_write(unavailable, sizeof(unavailable) - 1);
    }
    return exo_launch_start((uint64_t)ctx);
}

/* `run <package>` -- SCRUM-230. Reads `/<package>` off the program volume
 * (LIBOS_WAD_DISK_BASE_LBA, where build.sh's mkexofsimg step puts packaged
 * programs next to the IWAD) into a heap buffer and hands the bytes to the
 * kernel with exo_launch_pkg(). The kernel never sees a filesystem: this
 * shell is the launcher, which is the whole point of the syscall's shape.
 *
 * The volume is unmounted and the disk released BEFORE the launch, not
 * after: a successful exo_launch_pkg() does not come back until something
 * switches back to the shell, and holding the disk binding for the lifetime
 * of the launched program would lock every other context out of it. The
 * package buffer is freed after the call returns for the same reason it is
 * safe to: the kernel copies the sections into the new context's own pages
 * during the call. */
static void shell_run_run(fb_console_t *con, const char *arg) {
    char name[SHELL_FS_PATH_BUF_LEN];
    char path[SHELL_FS_PATH_BUF_LEN + 1];
    const char *p = arg;

    if (shell_next_token(&p, name, sizeof(name)) <= 0) {
        shell_print_usage(con, "run");
        return;
    }
    /* Deliberately not shell_resolve_path(): shell_cwd names a directory on
     * the shell's scratch volume, and this is a different volume. A name is
     * relative to the program volume's root; a leading '/' is accepted and
     * means the same thing, not "//name". */
    size_t n = 0;
    if (name[0] != '/') {
        path[n++] = '/';
    }
    size_t k = 0;
    while (name[k] != '\0' && n + 1 < sizeof(path)) {
        path[n++] = name[k++];
    }
    path[n] = '\0';
    if (name[k] != '\0') {
        /* Cut off: a truncated name is a different file. */
        fbcon_write(con, shell_fs_toolong_text);
        return;
    }

    if (shell_firstboot_classify(shell_probe_system_volume()) ==
        SHELL_FIRSTBOOT_UNFORMATTED) {
        fbcon_write(con, shell_nosys_text);
        fbcon_write(con, " (reboot to be offered a format)\n");
        return;
    }
    if (!shell_fs_acquire_and_mount_at(con, LIBOS_WAD_DISK_BASE_LBA)) {
        return;
    }

    /* SCRUM-270: the name as typed, or `install`'s reading of it. An exact
     * file wins; otherwise the best case-insensitive / ".exo" match at the
     * volume root (shell_run_match_rank()). The real name replaces `path`,
     * so the display name below is the file's, not what was typed. */
    {
        exofs_dir_t dir;
        int rc0 = exofs_opendir(shell_cur_vol, "/", &dir);
        if (rc0 != 0) {
            shell_fs_release();
            shell_err_report(con, SHELL_OP_FS, rc0);
            return;
        }
        int best = 0;
        char found[SHELL_FS_PATH_BUF_LEN + 1];
        for (;;) {
            exofs_dirinfo_t info;
            if (exofs_readdir(&dir, &info) <= 0) {
                break;
            }
            if (info.attributes & EXOFS_ATTR_DIRECTORY) {
                continue;
            }
            int r = shell_run_match_rank(path + 1, info.name);
            if (r > best) {
                best = r;
                size_t m = 0;
                found[m++] = '/';
                for (size_t q = 0; info.name[q] != '\0' && m + 1 < sizeof(found); q++) {
                    found[m++] = info.name[q];
                }
                found[m] = '\0';
                if (r == 3) {
                    break;
                }
            }
        }
        exofs_closedir(&dir);
        if (best == 0) {
            fbcon_write(con, shell_run_nosuch_text);
            fbcon_write(con, path + 1);
            fbcon_write(con, "\n");
            exofs_dir_t hd;
            if (exofs_opendir(shell_cur_vol, "/", &hd) == 0) {
                fbcon_write(con, shell_run_hint_text);
                if (!shell_list_programs(con, &hd)) {
                    fbcon_write(con, shell_programs_none_text);
                }
                exofs_closedir(&hd);
            }
            shell_fs_release();
            return;
        }
        for (size_t q = 0; ; q++) {
            path[q] = found[q];
            if (found[q] == '\0') {
                break;
            }
        }
    }

    exofs_stat_t st;
    int rc = exofs_stat(shell_cur_vol, path, &st);
    if (rc != 0) {
        shell_fs_release();
        shell_err_report(con, SHELL_OP_FS, rc);
        return;
    }

    /* Refuse a file the kernel could never accept before spending heap and
     * disk reads on it: a wrong-sized file is a bad argument, not ENOMEM. */
    if (st.size == 0 || st.size > EXO_LAUNCH_PKG_MAX_LEN) {
        shell_fs_release();
        shell_err_report(con, SHELL_OP_RUN, -EXO_EINVAL);
        return;
    }

    /* From the LibOS heap, which never returns pages to the system: the
     * buffer is reused by the next `run` (freed blocks coalesce), so a
     * repeated run does not keep growing the shell, but the high-water mark
     * of the largest package stays with it for the shell's lifetime. */
    uint8_t *pkg = libos_heap_alloc(st.size);
    if (pkg == NULL) {
        shell_fs_release();
        shell_err_report(con, SHELL_OP_RUN, -EXO_ENOMEM);
        return;
    }

    exofs_file_t f;
    rc = exofs_open(shell_cur_vol, path, EXOFS_O_READ, &f);
    uint32_t have = 0;
    if (rc == 0) {
        while (have < st.size) {
            int64_t got = exofs_read(&f, pkg + have, st.size - have);
            if (got <= 0) {
                rc = got < 0 ? (int)got : -EXO_EIO;
                break;
            }
            have += (uint32_t)got;
        }
        exofs_close(&f);
    }
    shell_fs_release();

    if (rc != 0 || have != st.size) {
        libos_heap_free(pkg);
        shell_err_report(con, SHELL_OP_FS, rc != 0 ? rc : -EXO_EIO);
        return;
    }

    /* The one exo_pkg_parse() in this file, shared by the admission check and
     * the grant read-out. It is a static inline from a header, and this blob
     * is linked with -fno-toplevel-reorder with byte 0 of the code as its
     * entry point (docker/scripts/build.sh): a second call site stops GCC
     * inlining it, and the out-of-line copy lands ahead of shell_main() and
     * is run as the entry. A second parse here faults the shell at boot. */
    exo_pkg_header_t hdr;
    const void *code, *data;
    int have_hdr = exo_pkg_parse(pkg, st.size, &hdr, &code, &data) == 0;

    if (!shell_admit_report(con, have_hdr ? &hdr : NULL)) {
        libos_heap_free(pkg);
        return;
    }

    /* The program's display name for pslist/the dashboard: the file name
     * without its directory or ".exo", cut to what the kernel records. */
    char prog_name[CONTEXT_NAME_MAX];
    {
        const char *base = path + 1;
        for (const char *q = base; *q != '\0'; q++) {
            if (*q == '/') { base = q + 1; }
        }
        size_t bl = 0;
        while (base[bl] != '\0' && bl < sizeof(prog_name) - 1) {
            prog_name[bl] = base[bl];
            bl++;
        }
        prog_name[bl] = '\0';
        if (bl > 4 && prog_name[bl - 4] == '.' && prog_name[bl - 3] == 'e' &&
            prog_name[bl - 2] == 'x' && prog_name[bl - 1] == 'o') {
            prog_name[bl - 4] = '\0';
        }
    }

    int64_t lrc = shell_launch_with_grant(pkg, st.size, have_hdr ? &hdr : NULL,
                                          prog_name);
    libos_heap_free(pkg);
    if (lrc == 0) {
        /* Back from the program (SCRUM-292). Nothing to redraw: the shell's
         * framebuffer is its own shadow (src/fb_shadow.h), which the program
         * never touched, so the earlier output is still there. Clearing here
         * was left over from the single shared framebuffer. */
    } else if (lrc == -EXO_ENOMEM) {
        /* The check passed on a snapshot taken before #36 loaded the IWAD
         * (doom_wad_disk_get() allocates it on first use), so the kernel's
         * backstop can still say no. Say why, with the figure that is true now. */
        exo_resstat_t res;
        fbcon_write(con, shell_admit_oom_text);
        shell_write_u32(con, exo_resstat(&res) == 0 ? res.free_pages : 0);
        fbcon_write(con, shell_admit_oom_tail_text);
    } else {
        shell_err_report(con, SHELL_OP_RUN, lrc);
    }
}

/* `mkfs` -- exofs_format() on the shell's own scratch volume (decision #1,
 * this file's own top comment). No mount needed afterwards: exofs_format()
 * does not leave the volume mounted, and this command has nothing further
 * to do with it. Destroys whatever the volume held before, same as any
 * other filesystem's mkfs.
 *
 * Sizes the volume from the disk's real capacity (exo_diskstat, #31) via
 * exofs_region_sectors(), rather than a hardcoded sector count -- SCRUM-203.
 * The region stops at LIBOS_WAD_DISK_BASE_LBA regardless of how big the
 * disk actually is: that LBA is where the WAD's own volume lives, and mkfs
 * must never claim sectors it owns. */
static int shell_format_volume(fb_console_t *con, uint32_t base_lba,
                               uint32_t region_end_lba) {
    int64_t rc = shell_disk_acquire_for(base_lba);
    if (rc != 0) {
        shell_err_report(con, SHELL_OP_DISK, rc);
        return 0;
    }

    exo_diskstat_t stat;
    int64_t src = exo_diskstat(SHELL_SYS_DEV, &stat);
    uint32_t vol_sectors = (src == 0)
        ? exofs_region_sectors(base_lba, region_end_lba, stat.total_sectors)
        : 0u;

    if (vol_sectors == 0u) {
        exo_disk_release(SHELL_SYS_DEV);
        fbcon_write(con, shell_fs_no_room_text);
        return 0;
    }

    int frc = exofs_format(SHELL_SYS_DEV, base_lba, vol_sectors);
    exo_disk_release(SHELL_SYS_DEV);

    if (frc != 0) {
        shell_err_report(con, SHELL_OP_FS, frc);
        return 0;
    }
    return 1;
}

static void shell_run_mkfs(fb_console_t *con, const char *arg) {
    (void)arg;
    if (shell_format_volume(con, SHELL_FS_BASE_LBA, LIBOS_WAD_DISK_BASE_LBA)) {
        fbcon_write(con, shell_fs_ok_text);
    }
}

/* Mount result for the system volume with the raw code exposed, which
 * shell_fs_acquire_and_mount_at() hides: first boot must tell "no superblock"
 * from "could not look". Acquires and releases the disk itself; on a failed
 * acquire the code is the acquire's. */
static int shell_probe_system_volume(void) {
    int64_t arc = shell_disk_acquire_for(LIBOS_WAD_DISK_BASE_LBA);
    if (arc != 0) {
        return (int)arc;
    }
    exofs_vol_t *pv = NULL;
    int mrc = exofs_mount(SHELL_SYS_DEV, LIBOS_WAD_DISK_BASE_LBA, &pv);
    if (mrc == 0) {
        exofs_unmount(pv);
    }
    exo_disk_release(SHELL_SYS_DEV);
    return mrc;
}

/* SCRUM-234: on startup, check the system volume (the program volume) and,
 * if the disk is blank, offer to format it. The decisions live in
 * shell_firstboot.h. A blank line, n, or any other key declines. */
static void shell_firstboot_check(fb_console_t *con) {
    int rc = shell_probe_system_volume();
    shell_firstboot_t state = shell_firstboot_classify(rc);

    if (state == SHELL_FIRSTBOOT_PRESENT) {
        return;
    }
    if (state == SHELL_FIRSTBOOT_ERROR) {
        shell_err_report(con, SHELL_OP_PROBE, rc);
        return;
    }

    fbcon_write(con, shell_firstboot_prompt_text);
    char answer = 0;
    for (;;) {
        exo_kbd_event_t ev;
        if (exo_kbd_poll(&ev) != 1 || !ev.pressed) {
            continue;
        }
        if (ev.key == KEY_ENTER) {
            break;
        }
        char c = shell_key_to_ascii(ev.key, ev.modifiers);
        if (c != 0) {
            answer = c;
            fbcon_putc(con, c);
            break;
        }
    }
    fbcon_write(con, "\n");

    if (!shell_firstboot_answer_is_yes(answer)) {
        fbcon_write(con, shell_firstboot_skip_text);
        return;
    }
    if (shell_format_volume(con, LIBOS_WAD_DISK_BASE_LBA,
                            EXO_VOLUME_REGION_BASE_LBA) &&
        shell_probe_system_volume() == 0) {
        fbcon_write(con, shell_firstboot_done_text);
    }
}

/* Print every *.exo file at the root of an open directory; returns how many. */
static int shell_list_programs(fb_console_t *con, exofs_dir_t *dir) {
    int found = 0;
    for (;;) {
        exofs_dirinfo_t info;
        if (exofs_readdir(dir, &info) <= 0) {
            break;
        }
        if (info.attributes & EXOFS_ATTR_DIRECTORY) {
            continue;
        }
        size_t n = 0;
        while (info.name[n] != '\0') {
            n++;
        }
        if (n < 5 || info.name[n - 4] != '.' || info.name[n - 3] != 'e' ||
            info.name[n - 2] != 'x' || info.name[n - 1] != 'o') {
            continue;
        }
        found = 1;
        fbcon_write(con, shell_ls_file_prefix);
        fbcon_write(con, info.name);
        fbcon_write(con, "\n");
    }
    return found;
}

/* `programs` -- the *.exo packages on the system volume. */
static void shell_run_programs(fb_console_t *con, const char *arg) {
    (void)arg;
    int rc = shell_probe_system_volume();
    if (shell_firstboot_classify(rc) == SHELL_FIRSTBOOT_UNFORMATTED) {
        fbcon_write(con, shell_nosys_text);
        fbcon_write(con, " (reboot to be offered a format)\n");
        return;
    }
    if (!shell_fs_acquire_and_mount_at(con, LIBOS_WAD_DISK_BASE_LBA)) {
        return;
    }

    exofs_dir_t dir;
    rc = exofs_opendir(shell_cur_vol, "/", &dir);
    if (rc != 0) {
        shell_fs_release();
        shell_err_report(con, SHELL_OP_FS, rc);
        return;
    }

    int found = shell_list_programs(con, &dir);
    exofs_closedir(&dir);
    shell_fs_release();

    if (!found) {
        fbcon_write(con, shell_programs_none_text);
    }
}

/* Mount the install disk's volume, or say why not. The caller holds the
 * device-1 binding; on success the volume is left mounted. */
static int shell_install_mount_src(fb_console_t *con) {
    int rc = exofs_mount(SHELL_SRC_DEV, SHELL_SRC_BASE_LBA, &shell_cur_vol);
    if (shell_firstboot_classify(rc) == SHELL_FIRSTBOOT_UNFORMATTED) {
        fbcon_write(con, shell_install_novol_text);
        return 0;
    }
    if (rc != 0) {
        shell_err_report(con, SHELL_OP_FS, rc);
        return 0;
    }
    return 1;
}

/* Fill `set` with the names that could complete the word `ctx` describes in
 * `line`. Opens, reads and closes whatever it needs; a failure to reach the
 * disk is reported by the shared mount helpers and leaves the set empty. */
static void shell_tab_gather(fb_console_t *con, const char *line,
                             const shell_comp_ctx_t *ctx,
                             shell_comp_set_t *set) {
    const char *prefix = line + ctx->leaf_off;
    size_t plen = ctx->leaf_len;
    exofs_dirinfo_t info;
    exofs_dir_t dir;

    shell_comp_init(set);

    switch (ctx->kind) {
    case SHELL_COMP_COMMAND:
        for (size_t i = 0; i < sizeof(shell_cmds) / sizeof(shell_cmds[0]); i++) {
            shell_comp_add(set, shell_cmds[i].help.name, 0, prefix, plen);
        }
        return;

    case SHELL_COMP_PROGRAM:
        if (shell_firstboot_classify(shell_probe_system_volume()) ==
                SHELL_FIRSTBOOT_UNFORMATTED ||
            !shell_fs_acquire_and_mount_at(con, LIBOS_WAD_DISK_BASE_LBA)) {
            return;
        }
        if (exofs_opendir(shell_cur_vol, "/", &dir) == 0) {
            while (exofs_readdir(&dir, &info) > 0) {
                if (!(info.attributes & EXOFS_ATTR_DIRECTORY) &&
                    shell_comp_is_program(info.name)) {
                    shell_comp_add(set, info.name, 0, prefix, plen);
                }
            }
            exofs_closedir(&dir);
        }
        shell_fs_release();
        return;

    case SHELL_COMP_INSTALL:
        if (exo_disk_acquire(SHELL_SRC_DEV, 0, 0) != 0) {
            return;     /* no install disk, or it is busy: nothing to offer */
        }
        if (shell_install_mount_src(con)) {
            if (exofs_opendir(shell_cur_vol, "/", &dir) == 0) {
                while (exofs_readdir(&dir, &info) > 0) {
                    if (!(info.attributes & EXOFS_ATTR_DIRECTORY)) {
                        shell_comp_add(set, info.name, 0, prefix, plen);
                    }
                }
                exofs_closedir(&dir);
            }
            exofs_unmount(shell_cur_vol);
        }
        exo_disk_release(SHELL_SRC_DEV);
        return;

    case SHELL_COMP_PATH:
    case SHELL_COMP_DIR: {
        char dir_path[SHELL_FS_PATH_BUF_LEN];
        if (ctx->dir_len >= sizeof(dir_path)) {
            return;
        }
        size_t n = 0;
        for (; n < ctx->dir_len; n++) {
            dir_path[n] = line[ctx->dir_off + n];
        }
        if (n == 0) {
            dir_path[n++] = '.';
        }
        dir_path[n] = '\0';

        shell_route_t rt;
        if (!shell_route_arg(con, dir_path, &rt) ||
            !shell_fs_acquire_and_mount_vol(con, &rt.vol)) {
            return;
        }
        if (exofs_opendir(shell_cur_vol, rt.inner, &dir) == 0) {
            while (exofs_readdir(&dir, &info) > 0) {
                int is_dir = (info.attributes & EXOFS_ATTR_DIRECTORY) != 0;
                if (ctx->kind == SHELL_COMP_DIR && !is_dir) {
                    continue;
                }
                shell_comp_add(set, info.name, is_dir, prefix, plen);
            }
            exofs_closedir(&dir);
        }
        shell_fs_release();
        return;
    }

    case SHELL_COMP_NONE:
    default:
        return;
    }
}

/* Tab (SCRUM-274). Gathers the candidates for the word at the end of the line
 * and appends what is unambiguous. If that added nothing and several
 * candidates remain, the set is still the answer for this line: the next Tab
 * (`again`) lists it instead of reading the disk again. Returns whether the
 * set can be reused that way; after a Tab that did change the line, the next
 * one gathers afresh against the new text. */
static int shell_tab(fb_console_t *con, char *cmd_buf, int *cmd_len,
                     shell_comp_set_t *set, int again) {
    if (again) {
        if (set->count > 1) {
            fbcon_write(con, "\n");
            for (unsigned i = 0; i < set->count; i++) {
                fbcon_write(con, set->name[i]);
                fbcon_write(con, set->is_dir[i] ? "/  " : "  ");
            }
            if (set->overflow) {
                fbcon_write(con, "...");
            }
            fbcon_write(con, "\n");
            shell_print_prompt(con);
            fbcon_write(con, cmd_buf);
        }
        return set->count > 1;
    }

    shell_comp_ctx_t ctx;
    shell_comp_classify(cmd_buf, (size_t)*cmd_len, &ctx);

    /* Gathering can print a diagnostic (a path that does not resolve, a disk
     * that will not mount). It lands after the half-typed line, so start a
     * fresh row and redraw the prompt and line below it. */
    unsigned row = con->cursor_y, col = con->cursor_x;
    shell_tab_gather(con, cmd_buf, &ctx, set);
    if (con->cursor_y != row || con->cursor_x != col) {
        if (con->cursor_x != 0) {
            fbcon_write(con, "\n");
        }
        shell_print_prompt(con);
        fbcon_write(con, cmd_buf);
        return 0;
    }

    char add[SHELL_COMP_NAME_LEN + 1];
    size_t n = shell_comp_apply(set, ctx.leaf_len, add, sizeof(add));
    for (size_t i = 0; i < n && *cmd_len < SHELL_CMD_BUF_LEN - 1; i++) {
        cmd_buf[(*cmd_len)++] = add[i];
        fbcon_putc(con, add[i]);
    }
    cmd_buf[*cmd_len] = '\0';
    return n == 0 && set->count > 1;
}

/* `install` with no argument: every file on the install disk's root, not
 * only packages -- the IWAD is installed the same way (`install
 * freedoom2.wad`). */
static void shell_install_list(fb_console_t *con) {
    if (!shell_install_mount_src(con)) {
        return;
    }
    exofs_dir_t dir;
    int rc = exofs_opendir(shell_cur_vol, "/", &dir);
    if (rc != 0) {
        exofs_unmount(shell_cur_vol);
        shell_err_report(con, SHELL_OP_FS, rc);
        return;
    }
    int found = 0;
    for (;;) {
        exofs_dirinfo_t info;
        if (exofs_readdir(&dir, &info) <= 0) {
            break;
        }
        if (info.attributes & EXOFS_ATTR_DIRECTORY) {
            continue;
        }
        found = 1;
        fbcon_write(con, shell_ls_file_prefix);
        fbcon_write(con, info.name);
        fbcon_write(con, "\n");
    }
    exofs_closedir(&dir);
    exofs_unmount(shell_cur_vol);
    if (!found) {
        fbcon_write(con, shell_install_empty_text);
    }
}

/* `install [name]` -- SCRUM-233. Holds both disks for the whole copy (one
 * extent per owner per device, docs/syscall_spec.md §3.5a): the install
 * disk whole, and the system disk's program volume exactly as `run` binds
 * it. shell_install_copy() mounts source and destination together for the
 * copy and always returns with nothing mounted, so the releases below never
 * drop a binding under a mounted volume (docs/filesystem.md §8). */
static void shell_run_install(fb_console_t *con, const char *arg) {
    char name[SHELL_FS_PATH_BUF_LEN];
    char path[SHELL_FS_PATH_BUF_LEN + 8];
    const char *p = arg;

    int tok = shell_next_token(&p, name, sizeof(name));
    if (tok < 0) {
        fbcon_write(con, shell_fs_toolong_text);
        return;
    }
    if (tok > 0 && shell_install_resolve(name, path, sizeof(path)) != 0) {
        /* Too long once ".exo" is added, or a bare "/" naming no file. */
        if (name[0] == '/' && name[1] == '\0') {
            shell_print_usage(con, "install");
        } else {
            fbcon_write(con, shell_fs_toolong_text);
        }
        return;
    }

    int64_t arc = exo_disk_acquire(SHELL_SRC_DEV, 0, 0);
    if (arc == -EXO_ENODEV) {
        fbcon_write(con, shell_install_nodev_text);
        return;
    }
    if (arc != 0) {
        shell_err_report(con, SHELL_OP_INSTALL, arc);
        return;
    }

    if (tok == 0) {
        shell_install_list(con);
        exo_disk_release(SHELL_SRC_DEV);
        return;
    }

    /* Report a missing source volume as that, not as a mount error from
     * somewhere in the middle of the copy. */
    if (!shell_install_mount_src(con)) {
        exo_disk_release(SHELL_SRC_DEV);
        return;
    }
    exofs_unmount(shell_cur_vol);

    if (shell_firstboot_classify(shell_probe_system_volume()) ==
        SHELL_FIRSTBOOT_UNFORMATTED) {
        exo_disk_release(SHELL_SRC_DEV);
        fbcon_write(con, shell_nosys_text);
        fbcon_write(con, " (reboot to be offered a format)\n");
        return;
    }
    arc = shell_disk_acquire_for(LIBOS_WAD_DISK_BASE_LBA);
    if (arc != 0) {
        exo_disk_release(SHELL_SRC_DEV);
        shell_err_report(con, SHELL_OP_DISK, arc);
        return;
    }

    shell_vol_t src = { SHELL_SRC_DEV, SHELL_SRC_BASE_LBA };
    shell_vol_t dst = { SHELL_SYS_DEV, LIBOS_WAD_DISK_BASE_LBA };

    /* What to copy: the one path, or every match of a pattern, listed in
     * full before the first copy so a failure part-way cannot change which
     * files the rest of the list names. */
    char *names = path;
    uint32_t count = 1;
    int rc = 0;
    int glob = shell_install_is_glob(path);
    if (glob) {
        names = libos_heap_alloc(SHELL_INSTALL_MAX_MATCHES *
                                 SHELL_INSTALL_NAME_STRIDE);
        rc = names == NULL ? -EXO_ENOMEM
                           : shell_install_matches(&src, path, names,
                                                   SHELL_INSTALL_NAME_STRIDE,
                                                   SHELL_INSTALL_MAX_MATCHES,
                                                   &count);
        if (rc == 0 && count == 0) {
            fbcon_write(con, shell_install_nomatch_text);
            fbcon_write(con, path);
            fbcon_write(con, "\n");
        }
    }

    uint32_t chunk = SHELL_INSTALL_CHUNK;
    uint8_t *buf = NULL;
    if (rc == 0 && count > 0) {
        buf = libos_heap_alloc(chunk);
        if (buf == NULL) {
            chunk = SHELL_INSTALL_CHUNK_SMALL;
            buf = libos_heap_alloc(chunk);
        }
        if (buf == NULL) {
            rc = -EXO_ENOMEM;
        }
    }

    uint32_t ok = 0;
    for (uint32_t i = 0; rc == 0 && i < count; i++) {
        const char *one = glob ? names + i * SHELL_INSTALL_NAME_STRIDE : path;
        uint32_t copied = 0;
        fbcon_write(con, shell_install_copying_text);
        fbcon_write(con, one);
        fbcon_write(con, "\n");
        int crc = shell_install_copy(&src, &dst, one, buf, chunk, &copied);
        if (crc != 0) {
            /* One bad file does not stop the rest of a pattern. */
            shell_err_report(con, SHELL_OP_FS, crc);
            continue;
        }
        ok++;
        fbcon_write(con, shell_install_done_text);
        fbcon_write(con, one);
        fbcon_write(con, " (");
        shell_write_u32(con, copied);
        fbcon_write(con, shell_install_bytes_text);
    }

    libos_heap_free(buf);
    if (glob) {
        libos_heap_free(names);
    }
    exo_disk_release(SHELL_SYS_DEV);
    exo_disk_release(SHELL_SRC_DEV);

    if (rc == -EXO_ENOSPC && glob) {
        fbcon_write(con, shell_install_toomany_text);
    } else if (rc == -EXO_EINVAL && glob) {
        fbcon_write(con, shell_install_toplevel_text);
    } else if (rc != 0) {
        shell_err_report(con, SHELL_OP_FS, rc);
    } else if (glob && count > 0) {
        fbcon_write(con, shell_install_done_text);
        shell_write_u32(con, ok);
        fbcon_write(con, shell_install_of_text);
        shell_write_u32(con, count);
        fbcon_write(con, shell_install_files_text);
    }
}

/* `mkdir <path>` -- exofs_mkdir(), against `arg` resolved relative to
 * shell_cwd if it is not already absolute. */
static void shell_run_mkdir(fb_console_t *con, const char *arg) {
    if (arg[0] == '\0') {
        shell_print_usage(con, "mkdir");
        return;
    }
    shell_route_t rt;
    if (!shell_route_arg(con, arg, &rt)) {
        return;
    }

    if (!shell_fs_acquire_and_mount_vol(con, &rt.vol)) {
        return;
    }
    int rc = exofs_mkdir(shell_cur_vol, rt.inner);
    shell_fs_release();

    if (rc != 0) {
        shell_err_report(con, SHELL_OP_FS, rc);
        return;
    }
    fbcon_write(con, shell_fs_ok_text);
}

/* `ls [path]` -- exofs_opendir()/exofs_readdir() loop, defaulting to
 * shell_cwd when no argument is given. Directories print with a trailing
 * "/" so mkdir'd entries are visually distinct from files, same spirit as
 * `ls -F`. */
static void shell_run_ls(fb_console_t *con, const char *arg) {
    shell_route_t rt;
    if (!shell_route_arg(con, arg[0] != '\0' ? arg : ".", &rt)) {
        return;
    }

    if (!shell_fs_acquire_and_mount_vol(con, &rt.vol)) {
        return;
    }

    exofs_dir_t dir;
    int rc = exofs_opendir(shell_cur_vol, rt.inner, &dir);
    if (rc != 0) {
        shell_fs_release();
        shell_err_report(con, SHELL_OP_FS, rc);
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
    const char *text = arg;
    int n = shell_next_token(&text, path_raw, sizeof(path_raw));
    if (n < 0) {
        fbcon_write(con, shell_fs_toolong_text);
        return;
    }
    if (n == 0) {
        shell_print_usage(con, "cat");
        return;
    }
    shell_route_t rt;
    if (!shell_route_arg(con, path_raw, &rt)) {
        return;
    }

    int writing = (*text != '\0');

    if (!shell_fs_acquire_and_mount_vol(con, &rt.vol)) {
        return;
    }

    exofs_file_t f;
    int orc = writing
        ? exofs_open(shell_cur_vol, rt.inner, EXOFS_O_WRITE | EXOFS_O_CREATE | EXOFS_O_TRUNC, &f)
        : exofs_open(shell_cur_vol, rt.inner, EXOFS_O_READ, &f);
    if (orc != 0) {
        shell_fs_release();
        shell_err_report(con, SHELL_OP_FS, orc);
        return;
    }

    if (writing) {
        uint32_t len = 0;
        while (text[len] != '\0') {
            len++;
        }
        int wrc = shell_write_all(&f, text, len);
        exofs_close(&f);
        shell_fs_release();
        if (wrc < 0) {
            shell_err_report(con, SHELL_OP_FS, wrc);
            return;
        }
        fbcon_write(con, shell_fs_ok_text);
        return;
    }

    char buf[64];
    for (;;) {
        int64_t got = exofs_read(&f, buf, sizeof(buf));
        if (got < 0) {
            shell_err_report(con, SHELL_OP_FS, got);
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
        shell_print_usage(con, "rm");
        return;
    }
    shell_route_t rt;
    if (!shell_route_arg(con, arg, &rt)) {
        return;
    }
    /* Removing the directory a volume is mounted on would pull it out from
     * under the mount table. */
    if (rt.mount_idx >= 0 && rt.inner[1] == '\0') {
        fbcon_write(con, shell_fs_mountpoint_text);
        return;
    }

    if (!shell_fs_acquire_and_mount_vol(con, &rt.vol)) {
        return;
    }

    int rc = exofs_unlink(shell_cur_vol, rt.inner);
    if (rc == -EXO_EISDIR) {
        rc = exofs_rmdir(shell_cur_vol, rt.inner);
    }
    shell_fs_release();

    if (rc != 0) {
        shell_err_report(con, SHELL_OP_FS, rc);
        return;
    }
    fbcon_write(con, shell_fs_ok_text);
}

/*
 * The copy behind `cp` and `mv` (`remove_src` set): within one volume,
 * shell_fs_copy_file(); across two, the chunked copy `install` uses
 * (shell_copy_vols()) with both volumes mounted for its whole length, which
 * needs both bound at once. The disk binding is one extent per device, so two volumes of the
 * same disk cannot be -- that is refused here, before anything is touched.
 * A refusal or a failed acquire is reported here and returns 1, so the
 * caller neither prints "error" nor "ok" on top; otherwise returns the first
 * error code (negative), or 0 for success.
 */
static int shell_fs_copy_or_move(fb_console_t *con, const shell_route_t *src,
                                 const shell_route_t *dst, int remove_src) {
    if (shell_same_vol(&src->vol, &dst->vol)) {
        if (!shell_fs_acquire_and_mount_vol(con, &src->vol)) {
            return 1;
        }
        int rc = shell_fs_copy_file(shell_cur_vol, src->inner, dst->inner);
        if (rc == 0 && remove_src) {
            rc = exofs_unlink(shell_cur_vol, src->inner);
        }
        shell_fs_release();
        return rc;
    }

    if (src->vol.dev == dst->vol.dev) {
        fbcon_write(con, shell_cp_samedisk_text);
        return 1;
    }

    int64_t arc = exo_disk_acquire(src->vol.dev, src->vol.extent_base,
                                   src->vol.extent_count);
    if (arc == 0) {
        arc = exo_disk_acquire(dst->vol.dev, dst->vol.extent_base,
                               dst->vol.extent_count);
        if (arc != 0) {
            exo_disk_release(src->vol.dev);
        }
    }
    if (arc != 0) {
        shell_err_report(con, SHELL_OP_DISK, arc);
        return 1;
    }

    uint32_t chunk = SHELL_INSTALL_CHUNK;
    uint8_t *buf = libos_heap_alloc(chunk);
    if (buf == NULL) {
        chunk = SHELL_INSTALL_CHUNK_SMALL;
        buf = libos_heap_alloc(chunk);
    }
    int rc = -EXO_ENOMEM;
    if (buf != NULL) {
        exofs_vol_t *sv = NULL, *dv = NULL;
        rc = exofs_mount(src->vol.dev, src->vol.base, &sv);
        if (rc == 0) {
            rc = exofs_mount(dst->vol.dev, dst->vol.base, &dv);
        }
        if (rc == 0) {
            rc = shell_copy_vols(sv, src->inner, dv, dst->inner, buf, chunk,
                                 NULL);
            if (rc == 0 && remove_src) {
                rc = exofs_unlink(sv, src->inner);
            }
        }
        exofs_unmount(dv);
        exofs_unmount(sv);
        libos_heap_free(buf);
    }
    exo_disk_release(src->vol.dev);
    exo_disk_release(dst->vol.dev);
    return rc;
}

/* `cp <src> <dst>` -- shell_fs_copy_file(), both paths resolved against
 * shell_cwd first. */
static void shell_run_cp(fb_console_t *con, const char *arg) {
    char src_raw[SHELL_FS_PATH_BUF_LEN], dst_raw[SHELL_FS_PATH_BUF_LEN];
    int split = shell_split_two(arg, src_raw, sizeof(src_raw),
                                dst_raw, sizeof(dst_raw));
    if (split < 0) {
        fbcon_write(con, shell_fs_toolong_text);
        return;
    }
    if (split == 0) {
        shell_print_usage(con, "cp");
        return;
    }
    shell_route_t src, dst;
    if (!shell_route_arg(con, src_raw, &src) ||
        !shell_route_arg(con, dst_raw, &dst)) {
        return;
    }

    int rc = shell_fs_copy_or_move(con, &src, &dst, 0);
    if (rc > 0) {
        return;
    }

    if (rc == -EXO_EEXIST) {
        fbcon_write(con, shell_fs_same_text);
        return;
    }
    if (rc != 0) {
        shell_err_report(con, SHELL_OP_FS, rc);
        return;
    }
    fbcon_write(con, shell_fs_ok_text);
}

/*
 * exofs_rename(), plus the overwrite `mv` has always done onto an existing
 * *file*: exofs_rename() never replaces, so the target is unlinked first and
 * the rename retried. That pair is not atomic: if the retry fails, the
 * target is already gone and the source stays where it was (the `mv` help
 * says so). The open-handle -EXO_EBUSY cannot be that failure: the shell
 * mounts a fresh volume per command, so nothing here holds a handle. Only
 * tried file-onto-file; a directory on either side keeps -EXO_EEXIST.
 */
static int shell_fs_rename(exofs_vol_t *v, const char *src, const char *dst) {
    int rc = exofs_rename(v, src, dst);
    if (rc != -EXO_EEXIST) {
        return rc;
    }
    exofs_stat_t st;
    if (exofs_stat(v, src, &st) != 0 ||
        (st.attributes & EXOFS_ATTR_DIRECTORY)) {
        return rc;
    }
    int urc = exofs_unlink(v, dst);
    if (urc != 0) {
        return urc == -EXO_EISDIR ? rc : urc;
    }
    return exofs_rename(v, src, dst);
}

/* `mv <src> <dst>`. Within one volume, shell_fs_rename(): a re-link, so
 * directories move too and nothing is copied. Between volumes there is no
 * entry to re-link, so it is `cp`'s copy and then a delete -- files only,
 * since exofs_unlink() on a directory source answers -EXO_EISDIR. */
static void shell_run_mv(fb_console_t *con, const char *arg) {
    char src_raw[SHELL_FS_PATH_BUF_LEN], dst_raw[SHELL_FS_PATH_BUF_LEN];
    int split = shell_split_two(arg, src_raw, sizeof(src_raw),
                                dst_raw, sizeof(dst_raw));
    if (split < 0) {
        fbcon_write(con, shell_fs_toolong_text);
        return;
    }
    if (split == 0) {
        shell_print_usage(con, "mv");
        return;
    }
    shell_route_t src, dst;
    if (!shell_route_arg(con, src_raw, &src) ||
        !shell_route_arg(con, dst_raw, &dst)) {
        return;
    }
    /* Moving a mount point away would leave the table pointing at nothing. */
    if (src.mount_idx >= 0 && src.inner[1] == '\0') {
        fbcon_write(con, shell_fs_mountpoint_text);
        return;
    }

    int rc;
    if (shell_same_vol(&src.vol, &dst.vol)) {
        /* The mount table and shell_cwd both hold paths, so moving a
         * directory that holds either would leave them naming something
         * that is no longer there. Same rule `umount` applies. */
        if (shell_mount_has_below(&shell_mounts, src.abs) ||
            shell_mount_path_within(shell_cwd, src.abs)) {
            fbcon_write(con, shell_mv_busy_text);
            return;
        }
        if (!shell_fs_acquire_and_mount_vol(con, &src.vol)) {
            return;
        }
        rc = shell_fs_rename(shell_cur_vol, src.inner, dst.inner);
        shell_fs_release();
        if (rc == -EXO_EEXIST) {
            fbcon_write(con, shell_mv_exists_text);
            return;
        }
    } else {
        rc = shell_fs_copy_or_move(con, &src, &dst, 1);
        if (rc > 0) {
            return;
        }
        if (rc == -EXO_EEXIST) {
            fbcon_write(con, shell_fs_same_text);
            return;
        }
    }

    if (rc != 0) {
        shell_err_report(con, SHELL_OP_FS, rc);
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
        shell_print_usage(con, "touch");
        return;
    }
    shell_route_t rt;
    if (!shell_route_arg(con, arg, &rt)) {
        return;
    }

    if (!shell_fs_acquire_and_mount_vol(con, &rt.vol)) {
        return;
    }
    exofs_file_t f;
    int rc = exofs_open(shell_cur_vol, rt.inner, EXOFS_O_WRITE | EXOFS_O_CREATE, &f);
    if (rc == 0) {
        exofs_close(&f);
    }
    shell_fs_release();

    if (rc != 0) {
        shell_err_report(con, SHELL_OP_FS, rc);
        return;
    }
    fbcon_write(con, shell_fs_ok_text);
}

/* `cd [path]` -- no arg goes to "/"; anything else is resolved against
 * shell_cwd, normalized ("." and ".." collapsed, including mid-path), and
 * routed to the volume it is on (SCRUM-266). The target is validated with
 * exofs_opendir() on that volume before shell_cwd is updated, so an invalid
 * target leaves the cwd unchanged. shell_cwd therefore always holds the
 * normalized absolute path, which `umount` relies on to tell whether the
 * shell is standing inside a mount. */
static void shell_run_cd(fb_console_t *con, const char *arg) {
    shell_route_t rt;
    if (!shell_route_arg(con, arg[0] == '\0' ? "/" : arg, &rt)) {
        return;
    }

    if (!shell_fs_acquire_and_mount_vol(con, &rt.vol)) {
        return;
    }
    exofs_dir_t dir;
    int rc = exofs_opendir(shell_cur_vol, rt.inner, &dir);
    if (rc == 0) {
        exofs_closedir(&dir);
    }
    shell_fs_release();

    if (rc != 0) {
        shell_err_report(con, SHELL_OP_FS, rc);
        return;
    }

    size_t i = 0;
    while (rt.abs[i] != '\0' && i < sizeof(shell_cwd) - 1) {
        shell_cwd[i] = rt.abs[i];
        i++;
    }
    shell_cwd[i] = '\0';
    fbcon_write(con, shell_fs_ok_text);
}

static void shell_run_pwd(fb_console_t *con, const char *arg) {
    (void)arg;
    fbcon_write(con, shell_cwd);
    fbcon_write(con, "\n");
}

/* `echo <text...>` -- no filesystem interaction, just prints its argument
 * back, same as `about`. */
static void shell_run_echo(fb_console_t *con, const char *arg) {
    fbcon_write(con, arg);
    fbcon_write(con, "\n");
}

/* One `disk` line: "disk N: S sectors (~M MiB)". */
static void shell_disk_line(fb_console_t *con, uint32_t dev,
                            const exo_diskstat_t *stat) {
    fbcon_write(con, "disk ");
    shell_write_u32(con, dev);
    fbcon_write(con, ": ");
    shell_write_u32(con, stat->total_sectors);
    fbcon_write(con, " sectors (~");
    shell_write_u32(con, stat->total_sectors / 2048u); /* 512B/sector -> MiB */
    fbcon_write(con, " MiB)\n");
}

static void shell_run_disk_system(fb_console_t *con,
                                  const exo_diskstat_t *stat);
static void shell_disk_volumes(fb_console_t *con, uint32_t dev);

/* `disk` -- exo_diskstat() (#31) for presence/capacity of every block device
 * (SCRUM-244: every index below EXO_DISK_MAX_DEVICES, since the system
 * disk's slot 0 can be empty while others are present), plus, for the
 * system disk, the WAD's LBA range this shell already knows about at compile
 * time (LIBOS_WAD_DISK_BASE_LBA, src/doom_wad_disk.h). The shell volume's
 * own size is computed live from the same capacity via
 * exofs_region_sectors() (SCRUM-203) -- the same number `mkfs` would format,
 * so the two commands can't disagree. */
static void shell_run_disk(fb_console_t *con, const char *arg) {
    (void)arg;
    exo_diskstat_t stat;
    if (exo_diskstat(SHELL_SYS_DEV, &stat) != 0) {
        fbcon_write(con, shell_disk_fail_text);
        return;
    }
    if (!stat.present) {
        fbcon_write(con, shell_disk_none_text);
    } else {
        shell_run_disk_system(con, &stat);
    }

    for (uint32_t dev = SHELL_SYS_DEV + 1u; dev < EXO_DISK_MAX_DEVICES; dev++) {
        if (exo_diskstat(dev, &stat) == 0 && stat.present) {
            shell_disk_line(con, dev, &stat);
            shell_disk_volumes(con, dev);
        }
    }
}

/* The volumes found on `dev` (shell_vol_list()), one line each, with the
 * directories they are mounted on. */
static void shell_disk_volumes(fb_console_t *con, uint32_t dev) {
    shell_volinfo_t vols[SHELL_VOL_MAX];
    uint32_t n = shell_vol_list(dev, vols, SHELL_VOL_MAX);
    for (uint32_t i = 0; i < n; i++) {
        fbcon_write(con, "  vol");
        shell_write_u32(con, vols[i].ref.volno);
        fbcon_write(con, ": ");
        shell_vol_describe(con, &vols[i]);
        fbcon_write(con, ", lba ");
        shell_write_u32(con, vols[i].ref.base);
        fbcon_write(con, "+");
        if (vols[i].busy) {
            fbcon_write(con, " (in use by a program)");
        }
        for (uint32_t m = 0; m < SHELL_MOUNT_MAX; m++) {
            if (shell_mounts.e[m].used &&
                shell_same_vol(&shell_mounts.e[m].vol, &vols[i].ref)) {
                fbcon_write(con, " -> ");
                fbcon_write(con, shell_mounts.e[m].point);
            }
        }
        fbcon_write(con, "\n");
    }
}

/* The system disk's `disk` lines: its size, then its volumes. The shell
 * volume's own size is computed live from the same capacity via
 * exofs_region_sectors() (SCRUM-203) -- the same number `mkfs` would format
 * -- so the two commands can't disagree. */
static void shell_run_disk_system(fb_console_t *con,
                                  const exo_diskstat_t *stat) {
    shell_disk_line(con, SHELL_SYS_DEV, stat);
    shell_disk_volumes(con, SHELL_SYS_DEV);
    fbcon_write(con, "  mkfs formats ");
    shell_write_u32(con, exofs_region_sectors(SHELL_FS_BASE_LBA,
                                              LIBOS_WAD_DISK_BASE_LBA,
                                              stat->total_sectors));
    fbcon_write(con, " sectors at lba ");
    shell_write_u32(con, SHELL_FS_BASE_LBA);
    fbcon_write(con, "\n");
}

/* `mount` with no arguments: what is mounted where. */
static void shell_mount_list(fb_console_t *con) {
    int any = 0;
    for (uint32_t m = 0; m < SHELL_MOUNT_MAX; m++) {
        if (!shell_mounts.e[m].used) {
            continue;
        }
        any = 1;
        fbcon_write(con, shell_mounts.e[m].point);
        fbcon_write(con, " <- disk");
        shell_write_u32(con, shell_mounts.e[m].vol.dev);
        fbcon_write(con, ":vol");
        shell_write_u32(con, shell_mounts.e[m].vol.volno);
        fbcon_write(con, "\n");
    }
    if (!any) {
        fbcon_write(con, shell_mount_none_text);
    }
}

/* Whether the directory `path` on the volume `v` holds nothing but "." and
 * "..". Returns 1 empty, 0 not empty, or a negative error from the
 * filesystem (no such directory, not a directory, ...). If the volume could
 * not be mounted at all it has already said so and this returns 2. */
static int shell_dir_is_empty(fb_console_t *con, const shell_volref_t *v,
                              const char *path) {
    if (!shell_fs_acquire_and_mount_vol(con, v)) {
        return 2;
    }
    exofs_dir_t dir;
    int rc = exofs_opendir(shell_cur_vol, path, &dir);
    if (rc == 0) {
        rc = 1;
        for (;;) {
            exofs_dirinfo_t info;
            int got = exofs_readdir(&dir, &info);
            if (got < 0) {
                rc = got;
                break;
            }
            if (got == 0) {
                break;
            }
            if (!str_eq(info.name, ".") && !str_eq(info.name, "..")) {
                rc = 0;
                break;
            }
        }
        exofs_closedir(&dir);
    }
    shell_fs_release();
    return rc;
}

/* `mount diskN:volM <dir>` -- attach a volume `disk` lists to an existing,
 * empty directory. Only records it (shell_mount_add()): the volume is
 * actually mounted, per command, when a path routes to it. */
static void shell_run_mount(fb_console_t *con, const char *arg) {
    if (arg[0] == '\0') {
        shell_mount_list(con);
        return;
    }

    char vname[SHELL_FS_PATH_BUF_LEN], dir_raw[SHELL_FS_PATH_BUF_LEN];
    int split = shell_split_two(arg, vname, sizeof(vname),
                                dir_raw, sizeof(dir_raw));
    if (split < 0) {
        fbcon_write(con, shell_fs_toolong_text);
        return;
    }
    if (split == 0) {
        shell_print_usage(con, "mount");
        return;
    }

    uint32_t disk, volno;
    if (shell_mount_parse_volname(vname, &disk, &volno) != 0) {
        fbcon_write(con, shell_mount_badname_text);
        return;
    }
    shell_volinfo_t vols[SHELL_VOL_MAX];
    uint32_t n = disk < EXO_DISK_MAX_DEVICES
                     ? shell_vol_list(disk, vols, SHELL_VOL_MAX)
                     : 0;
    const shell_volinfo_t *vi = NULL;
    for (uint32_t k = 0; k < n; k++) {
        if (vols[k].ref.volno == volno) {
            vi = &vols[k];
        }
    }
    if (vi == NULL) {
        fbcon_write(con, shell_mount_novol_text);
        fbcon_write(con, vname);
        fbcon_write(con, "\n");
        return;
    }
    /* The volume `/` already is. Mounting it again would put the shell's
     * own files under one of its own directories, so a path could reach the
     * same volume two ways and the mount point would be inside what it
     * mounts. */
    shell_volref_t self = shell_sys_volref(SHELL_FS_BASE_LBA);
    if (shell_same_vol(&vi->ref, &self)) {
        fbcon_write(con, shell_mount_self_text);
        return;
    }
    if (vi->busy) {
        fbcon_write(con, shell_mount_inuse_text);
        fbcon_write(con, vname);
        fbcon_write(con, "\n");
        return;
    }

    shell_route_t rt;
    if (!shell_route_arg(con, dir_raw, &rt)) {
        return;
    }
    if (rt.abs[1] == '\0') {
        fbcon_write(con, shell_mount_root_text);
        return;
    }
    if (shell_mount_find(&shell_mounts, rt.abs) >= 0) {
        fbcon_write(con, shell_mount_already_text);
        return;
    }

    int empty = shell_dir_is_empty(con, &rt.vol, rt.inner);
    if (empty == 2) {
        return;
    }
    if (empty == 0) {
        fbcon_write(con, shell_mount_notempty_text);
        return;
    }
    if (empty < 0) {
        shell_err_report(con, SHELL_OP_FS, empty);
        return;
    }

    int rc = shell_mount_add(&shell_mounts, rt.abs, &vi->ref);
    if (rc != 0) {
        shell_err_report(con, SHELL_OP_FS, rc);
        return;
    }
    fbcon_write(con, shell_fs_ok_text);
}

/* `umount <dir>` -- forget a mount. */
static void shell_run_umount(fb_console_t *con, const char *arg) {
    if (arg[0] == '\0') {
        shell_print_usage(con, "umount");
        return;
    }
    shell_route_t rt;
    if (!shell_route_arg(con, arg, &rt)) {
        return;
    }
    int rc = shell_mount_remove(&shell_mounts, rt.abs, shell_cwd);
    if (rc == -EXO_EINVAL) {
        fbcon_write(con, shell_umount_notmnt_text);
        return;
    }
    if (rc == -EXO_EBUSY) {
        fbcon_write(con, shell_umount_busy_text);
        return;
    }
    fbcon_write(con, shell_fs_ok_text);
}

/* Next token of a `dd` command line as a uint32_t (shell_parse_u32()).
 * Prints usage for a missing token and names the offending text for a bad
 * one; returns 1 with *out set, or 0 (already reported). */
static int shell_dd_number(fb_console_t *con, const char **p, char *tok,
                           size_t tok_len, uint32_t *out) {
    int n = shell_next_token(p, tok, tok_len);
    if (n == 0) {
        shell_print_usage(con, "dd");
        return 0;
    }
    if (n < 0 || !shell_parse_u32(tok, out)) {
        fbcon_write(con, shell_dd_badnum_text);
        fbcon_write(con, n < 0 ? "(too long)" : tok);
        fbcon_write(con, "\n");
        return 0;
    }
    return 1;
}

/* `dd read <lba> <count> <path>` / `dd write <path> <lba>` -- raw sector
 * I/O, bypassing ExoFS entirely for the disk side. `dd read` may read any
 * LBA, the IWAD volume included; `dd write` is range-checked (see
 * shell_run_dd_write()). Just dispatches on the sub-verb; see
 * shell_run_dd_read()/_write() for the actual transfers. */
static void shell_run_dd(fb_console_t *con, const char *arg) {
    const char *rest;
    if (shell_cmd_match(arg, "read", &rest)) {
        shell_run_dd_read(con, rest);
    } else if (shell_cmd_match(arg, "write", &rest)) {
        shell_run_dd_write(con, rest);
    } else {
        shell_print_usage(con, "dd");
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
    uint32_t lba, count;

    if (!shell_dd_number(con, &p, tok, sizeof(tok), &lba) ||
        !shell_dd_number(con, &p, tok, sizeof(tok), &count)) {
        return;
    }

    if (*p == '\0') {
        shell_print_usage(con, "dd");
        return;
    }
    char path[SHELL_FS_RESOLVED_BUF_LEN];
    if (!shell_resolve_arg(con, p, path, sizeof(path))) {
        return;
    }

    if (count == 0) {
        fbcon_write(con, shell_fs_ok_text);
        return;
    }

    if (!shell_fs_acquire_and_mount(con)) {
        return;
    }

    exofs_file_t f;
    int rc = exofs_open(shell_cur_vol, path, EXOFS_O_WRITE | EXOFS_O_CREATE | EXOFS_O_TRUNC,
                        &f);
    if (rc != 0) {
        shell_fs_release();
        shell_err_report(con, SHELL_OP_FS, rc);
        return;
    }

    uint8_t sector[512];
    for (uint32_t i = 0; i < count; i++) {
        int64_t got = exo_disk_read(SHELL_SYS_DEV, lba + i, sector, 1);
        if (got < 0) {
            rc = (int)got;
            break;
        }
        rc = shell_write_all(&f, sector, sizeof(sector));
        if (rc != 0) {
            break;
        }
    }
    exofs_close(&f);
    shell_fs_release();

    if (rc != 0) {
        shell_err_report(con, SHELL_OP_FS, rc);
        return;
    }
    fbcon_write(con, shell_fs_ok_text);
}

/* `dd write <path> <lba>` -- the inverse: reads `path` back out through
 * ExoFS and writes it, 512 bytes at a time, to raw sectors starting at
 * `lba`. Sector count is derived from the file (ceil(size / 512)), not
 * passed in -- a final short read is zero-padded so the last sector is
 * still a full 512-byte write.
 *
 * The whole target range must lie below SHELL_FS_BASE_LBA, checked before
 * the first sector is written. Above that is this shell's own volume --
 * mounted for the duration of this very command, so its FAT cache would be
 * flushed back over the raw bytes on unmount -- and then the IWAD volume at
 * LIBOS_WAD_DISK_BASE_LBA, which `doom` reads for the rest of the boot. */
static void shell_run_dd_write(fb_console_t *con, const char *rest) {
    char path_raw[SHELL_FS_PATH_BUF_LEN];
    const char *p = rest;

    int n = shell_next_token(&p, path_raw, sizeof(path_raw));
    if (n < 0) {
        fbcon_write(con, shell_fs_toolong_text);
        return;
    }
    if (n == 0) {
        shell_print_usage(con, "dd");
        return;
    }
    char tok[16];
    uint32_t lba;
    if (!shell_dd_number(con, &p, tok, sizeof(tok), &lba)) {
        return;
    }

    char path[SHELL_FS_RESOLVED_BUF_LEN];
    if (!shell_resolve_arg(con, path_raw, path, sizeof(path))) {
        return;
    }

    if (!shell_fs_acquire_and_mount(con)) {
        return;
    }

    exofs_file_t f;
    int rc = exofs_open(shell_cur_vol, path, EXOFS_O_READ, &f);
    if (rc != 0) {
        shell_fs_release();
        shell_err_report(con, SHELL_OP_FS, rc);
        return;
    }

    /* 64-bit so neither the round-up nor lba + sectors can wrap. */
    uint64_t sectors = ((uint64_t)f.size + 511u) / 512u;
    if ((uint64_t)lba + sectors > SHELL_FS_BASE_LBA) {
        exofs_close(&f);
        shell_fs_release();
        fbcon_write(con, shell_dd_range_text);
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
        int64_t wrote = exo_disk_write(SHELL_SYS_DEV, lba + i, sector, 1);
        if (wrote < 0) {
            rc = (int)wrote;
            break;
        }
        i++;
    }
    exofs_close(&f);
    shell_fs_release();

    if (rc != 0) {
        shell_err_report(con, SHELL_OP_FS, rc);
        return;
    }
    fbcon_write(con, shell_fs_ok_text);
}

/* `memstat` -- exo_memstat (#22, src/syscall_stat.c). The output struct is
 * an ordinary local: the shell's own stack is always in the LibOS window
 * and mapped (libos_build_image() put it there), so the -EXO_EFAULT branch
 * below is unreachable in practice and only guards against the kernel ever
 * disagreeing with that. */
static void shell_run_memstat(fb_console_t *con, const char *arg) {
    (void)arg;
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
static void shell_run_pslist(fb_console_t *con, const char *arg) {
    (void)arg;
    exo_ps_info_t procs[EXO_PSLIST_MAX];
    int64_t rc = exo_pslist(procs, EXO_PSLIST_MAX);
    if (rc < 0) {
        fbcon_write(con, shell_pslist_fail_text);
        return;
    }

    for (int64_t i = 0; i < rc; i++) {
        fbcon_write(con, "id=");
        shell_write_u32(con, procs[i].id);
        if (procs[i].name[0] != '\0') {
            fbcon_write(con, " name=");
            fbcon_write(con, procs[i].name);
        }
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
