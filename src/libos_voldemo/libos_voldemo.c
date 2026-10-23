/*
 * libos_voldemo.c — per-program filesystem volume demo (SCRUM-237), ring 3.
 *
 * The live half of SCRUM-237's acceptance: this one program is packaged twice
 * (voldemoa.exo / voldemob.exo, docker/scripts/build.sh), once per volume
 * slot, so two copies can run at the same time, each with a filesystem of its
 * own. Nothing here is special-cased per copy -- the program asks the kernel
 * which sectors are its own (exo_disk_extent, #37), binds exactly those
 * (exo_disk_acquire, #26), links its own copy of ExoFS (src/libos_fs/) and
 * mounts it at that base. The kernel never learns there is a filesystem; it
 * only ever sees exo_disk_read/exo_disk_write on sectors the caller holds.
 *
 * On screen, in order: the extent, whether the volume was mounted or had to
 * be formatted (first launch), a file this copy writes into its own root, the
 * root listing -- which shows that copy's files and nobody else's -- and then
 * the part that makes it a demonstration rather than a coincidence: it tries
 * to read the first sector of every *other* volume slot, the shell's scratch
 * volume and the program volume, and tries to claim the whole disk. Every one
 * of those is refused by the kernel. A launch counter in the volume shows it
 * persists across runs.
 *
 * Every result is mirrored to COM1 as `voldemo[<volume>] ...` lines so the
 * isolation can be checked from a headless boot, not only read off the screen.
 *
 * Which copy is which comes from the slot the kernel assigned, not from a
 * compiled-in name, so there is one binary and one source of truth. Ctrl+Tab
 * switches between the two copies (and the shell) the same way it does for
 * every other LibOS; Q/Esc releases the volume and exits.
 *
 * libos_voldemo_main() carries __attribute__((section(".text.entry"))) so it
 * lands at offset 0 of the linked .text blob -- same convention and reason as
 * libos_clock.c -- and this file must be listed first in build.sh's source
 * list for this target.
 */

#include "exo_syscall.h"
#include "exo_volume.h"
#include "libos_fb.h"
#include "libos_heap.h"
#include "libos_fs/exofs.h"
#include "fb.h"
#include "fb_console.h"
#include "ps2.h"   /* ps2_key_t (KEY_Q/KEY_ESC) only */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

__attribute__((section(".text.entry")))
void libos_voldemo_main(void);

/* .data, not .rodata -- build_ring3_link_target's objcopy step errors on an
 * empty .data extraction (see libos_clock.c). */
static char voldemo_banner[]   = "ExoDoom per-program volume demo\n";
static char voldemo_launches[] = "/launches";
static char voldemo_message[]  = "this file lives in my own volume\n";

static void write_u32(fb_console_t *con, uint32_t val)
{
    if (val == 0) { fbcon_write(con, "0"); return; }
    char buf[11];
    buf[10] = '\0';
    int i = 10;
    while (val > 0) { buf[--i] = (char)('0' + (val % 10)); val /= 10; }
    fbcon_write(con, &buf[i]);
}

static void write_i64(fb_console_t *con, int64_t val)
{
    if (val < 0) {
        fbcon_write(con, "-");
        write_u32(con, (uint32_t)(-val));
    } else {
        write_u32(con, (uint32_t)val);
    }
}

static void label(fb_console_t *con, const char *text)
{
    fbcon_set_color(con, 220, 220, 220, 0, 0, 0);
    fbcon_write(con, text);
}

static void value(fb_console_t *con, const char *text)
{
    fbcon_set_color(con, 255, 220, 80, 0, 0, 0);
    fbcon_write(con, text);
}

/* Nothing we can do with a failure but say so and stay put; the user can
 * Ctrl+Tab away. */
static void voldemo_halt(fb_console_t *con, const char *what, int64_t rc)
{
    fbcon_set_color(con, 255, 90, 90, 0, 0, 0);
    fbcon_write(con, what);
    write_i64(con, rc);
    fbcon_write(con, "\n");
    for (;;) { exo_yield(); }
}

/* Read this copy's launch counter from its own volume, bump it, store it
 * back. A fresh volume has no such file, which reads as zero. */
static uint32_t voldemo_bump_launches(void)
{
    uint32_t n = 0;
    exofs_file_t f;

    if (exofs_open(voldemo_launches, EXOFS_O_READ, &f) == 0) {
        if (exofs_read(&f, &n, sizeof(n)) != (int64_t)sizeof(n))
            n = 0;
        exofs_close(&f);
    }
    n++;
    if (exofs_open(voldemo_launches, EXOFS_O_WRITE | EXOFS_O_CREATE |
                                         EXOFS_O_TRUNC, &f) == 0) {
        exofs_write(&f, &n, sizeof(n));
        exofs_close(&f);
    }
    return n;
}

/* Try to read sector `lba` and report what the kernel said. A refusal is the
 * expected answer for everything except our own extent. */
static void voldemo_probe(fb_console_t *con, const char *vol, const char *what,
                          uint32_t lba, void *buf)
{
    int64_t rc = exo_disk_read(lba, buf, 1);

    printf("voldemo[%s] probe %s lba=%u rc=%d %s\n", vol, what, (unsigned)lba,
           (int)rc, rc < 0 ? "refused" : "READ-OK");

    fbcon_write(con, "  ");
    label(con, what);
    fbcon_write(con, " (LBA ");
    write_u32(con, lba);
    fbcon_write(con, "): ");
    if (rc < 0) {
        fbcon_set_color(con, 90, 255, 120, 0, 0, 0);
        fbcon_write(con, "refused (");
        write_i64(con, rc);
        fbcon_write(con, ")\n");
    } else {
        fbcon_set_color(con, 255, 90, 90, 0, 0, 0);
        fbcon_write(con, "READ OK -- isolation broken\n");
    }
}

__attribute__((section(".text.entry")))
void libos_voldemo_main(void)
{
    libos_fb_t libfb;
    if (libos_fb_map(&libfb) != 0 || libfb.vaddr == NULL) {
        for (;;) { exo_yield(); }
    }

    framebuffer_t fb;
    fb_console_t con;
    if (!fb_init_bgrx8888(&fb, (uintptr_t)libfb.vaddr, libfb.pitch,
                          libfb.width, libfb.height, libfb.bpp) ||
        !fbcon_init(&con, &fb)) {
        for (;;) { exo_yield(); }
    }

    fbcon_set_color(&con, 100, 220, 255, 0, 0, 0);
    fbcon_write(&con, voldemo_banner);

    exo_diskextent_t ext;
    int64_t rc = exo_disk_extent(&ext);
    if (rc != 0)
        voldemo_halt(&con, "no volume assigned (launch from a package that "
                           "requests one): ", rc);

    uint32_t slot = (ext.base - EXO_VOLUME_REGION_BASE_LBA) /
                    EXO_VOLUME_SLOT_SECTORS;
    char name[2] = { (char)('A' + slot), '\0' };
    /* The file name follows the slot too, so any slot can run this program. */
    char hello[] = "/hello-?.txt";
    hello[7] = (char)('a' + slot);

    printf("voldemo[%s] extent base=%u sectors=%u\n", name, (unsigned)ext.base,
           (unsigned)ext.sectors);

    label(&con, "volume   ");
    value(&con, name);
    label(&con, "   extent  LBA ");
    write_u32(&con, ext.base);
    label(&con, " +");
    write_u32(&con, ext.sectors);
    label(&con, " sectors\n");

    rc = exo_disk_acquire(ext.base, ext.sectors);
    if (rc != 0)
        voldemo_halt(&con, "exo_disk_acquire failed: ", rc);

    label(&con, "fs       ");
    if (exofs_mount(ext.base) == 0) {
        printf("voldemo[%s] mounted existing volume\n", name);
        value(&con, "mounted existing volume\n");
    } else {
        int frc = exofs_format(ext.base, ext.sectors);
        if (frc != 0)
            voldemo_halt(&con, "exofs_format failed: ", frc);
        int mrc = exofs_mount(ext.base);
        if (mrc != 0)
            voldemo_halt(&con, "exofs_mount failed: ", mrc);
        printf("voldemo[%s] formatted new volume\n", name);
        value(&con, "first launch -- formatted a new volume\n");
    }

    label(&con, "launches ");
    uint32_t launches = voldemo_bump_launches();
    printf("voldemo[%s] launches=%u\n", name, (unsigned)launches);
    write_u32(&con, launches);
    fbcon_write(&con, " (kept in this volume)\n");

    /* Each copy writes a differently-named file, so that if the volumes were
     * ever shared the other copy's file would show up in the listing below. */
    exofs_file_t f;
    if (exofs_open(hello, EXOFS_O_WRITE | EXOFS_O_CREATE | EXOFS_O_TRUNC,
                   &f) == 0) {
        exofs_write(&f, voldemo_message, sizeof(voldemo_message) - 1);
        exofs_close(&f);
    }
    exofs_sync();

    label(&con, "root     ");
    fbcon_write(&con, "\n");
    exofs_dir_t dir;
    if (exofs_opendir("/", &dir) == 0) {
        for (;;) {
            exofs_dirinfo_t info;
            if (exofs_readdir(&dir, &info) <= 0)
                break;
            printf("voldemo[%s] file %s size=%u\n", name, info.name,
                   (unsigned)info.size);
            fbcon_write(&con, "  ");
            value(&con, info.name);
            fbcon_write(&con, " (");
            write_u32(&con, info.size);
            fbcon_write(&con, " bytes)\n");
        }
        exofs_closedir(&dir);
    }

    fbcon_set_color(&con, 100, 220, 255, 0, 0, 0);
    fbcon_write(&con, "reaching for what is not mine:\n");
    void *probe = libos_heap_alloc(512);
    if (probe) {
        for (uint32_t s = 0; s < EXO_VOLUME_MAX_SLOTS; s++) {
            if (s == slot)
                continue;
            char other[8] = { 'v', 'o', 'l', 'u', 'm', 'e', (char)('A' + s), 0 };
            voldemo_probe(&con, name, other, exo_volume_slot_base(s), probe);
        }
        voldemo_probe(&con, name, "shell-scratch", 8192, probe);
        voldemo_probe(&con, name, "program-volume", 16384, probe);
    }
    rc = exo_disk_acquire(0, 0);
    printf("voldemo[%s] probe whole-disk rc=%d %s\n", name, (int)rc,
           rc < 0 ? "refused" : "GRANTED");
    fbcon_write(&con, "  ");
    label(&con, "whole disk");
    fbcon_write(&con, ": ");
    if (rc < 0) {
        fbcon_set_color(&con, 90, 255, 120, 0, 0, 0);
        fbcon_write(&con, "refused (");
        write_i64(&con, rc);
        fbcon_write(&con, ")\n");
    } else {
        fbcon_set_color(&con, 255, 90, 90, 0, 0, 0);
        fbcon_write(&con, "GRANTED -- isolation broken\n");
    }

    fbcon_set_color(&con, 140, 140, 140, 0, 0, 0);
    fbcon_write(&con, "Ctrl+Tab to switch, Q/Esc to exit\n");

    for (;;) {
        exo_kbd_event_t ev;
        if (exo_kbd_poll(&ev) > 0 && ev.pressed &&
            (ev.key == KEY_Q || ev.key == KEY_ESC)) {
            exofs_unmount();
            exo_disk_release();
            exo_syscall1(EXO_SYS_EXIT, 0);
            for (;;) { exo_yield(); }
        }
        for (volatile uint32_t spin = 0; spin < 20000; spin++) {
            __asm__ volatile ("pause");
        }
    }
}
