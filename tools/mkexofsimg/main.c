/*
 * mkexofsimg — host-side ExoFS disk-image builder (SCRUM-190).
 *
 * Builds build/exodoom_disk.img: an ExoFS volume holding one file, the
 * freedoom2.wad IWAD, so the kernel can mount it through
 * exo_disk_read/exo_disk_write + src/libos_fs/ instead of the old GRUB
 * multiboot2 module. Links every src/libos_fs/ source directly (except
 * exofs_blockdev.c, whose job is the syscall seam this tool replaces with
 * host_blockdev.c), against the real exofs_layout.h/exofs.h --
 * exofs_layout.h's own header comment calls out this tool by name as the
 * reason it carries no dependency beyond <stdint.h>/<stddef.h>.
 *
 * The volume starts at a caller-supplied base_lba, not LBA 0: this one
 * image is attached to every QEMU invocation now, test and normal boot
 * alike (src/ata.c only ever speaks to one drive), so the low LBAs below
 * LIBOS_WAD_DISK_BASE_LBA (src/doom_wad_disk.h) are left zeroed and
 * untouched -- that is exactly the range the pre-SCRUM-190 raw-sector/ExoFS
 * KUnit fixtures (test_ata_k.c, test_syscall_disk_k.c, test_exofs_k.c)
 * already reserve on the image this one replaces, build/ata_scratch.img.
 * SCRUM-237 (per-program filesystem isolation) reuses this exact mechanism
 * with a different base_lba per program.
 *
 * Deliberately compiled WITHOUT "-I src": this file uses the real
 * <stdio.h>/<stdlib.h>, and "-I src" would shadow them with the project's
 * freestanding shims (see host_blockdev.c's and host_libos_heap.c's own
 * comments for the same reasoning). It needs only "exofs.h" from the
 * project tree, found via "-I src/libos_fs".
 */

#include "exofs.h"
#include "host_blockdev.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 2 MiB of slack above the WAD's own byte count: enough for the FAT
 * (4 bytes/block, so ~framework for a volume this small is a few hundred
 * sectors), the superblock and root block, and headroom so the image
 * isn't a hairline fit. See EXOFS_MAX_BLOCKS's own comment (src/libos_fs/
 * exofs.h) for why this is nowhere near that ceiling. */
#define SLACK_SECTORS (4096u)

/* strtoul() alone can't tell "0" from "garbage" or "4294967296" from
 * "4294967295 trailing junk" -- all three otherwise land on base_lba = 0.
 * Reject anything that isn't purely decimal digits or that overflows
 * uint32_t rather than silently misplacing the volume on disk. */
static int parse_base_lba(const char *s, uint32_t *out)
{
    if (*s == '\0') {
        return -1;
    }
    errno = 0;
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    if (*end != '\0' || errno == ERANGE || v > UINT32_MAX) {
        return -1;
    }
    *out = (uint32_t)v;
    return 0;
}

static int write_file_into_volume(const char *host_path,
                                   const char *volume_path)
{
    FILE *in = fopen(host_path, "rb");
    if (!in) {
        perror(host_path);
        return -1;
    }

    exofs_file_t f;
    int rc = exofs_open(volume_path,
                        EXOFS_O_WRITE | EXOFS_O_CREATE | EXOFS_O_TRUNC, &f);
    if (rc != 0) {
        fprintf(stderr, "mkexofsimg: exofs_open(%s) failed: %d\n",
                volume_path, rc);
        fclose(in);
        return -1;
    }

    uint8_t buf[65536];
    size_t  n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        int64_t w = exofs_write(&f, buf, (uint32_t)n);
        if (w < 0 || (size_t)w != n) {
            fprintf(stderr, "mkexofsimg: exofs_write failed: %lld\n",
                    (long long)w);
            exofs_close(&f);
            fclose(in);
            return -1;
        }
    }
    int had_error = ferror(in);
    fclose(in);
    if (had_error) {
        fprintf(stderr, "mkexofsimg: error reading %s\n", host_path);
        exofs_close(&f);
        return -1;
    }

    rc = exofs_close(&f);
    if (rc != 0) {
        fprintf(stderr, "mkexofsimg: exofs_close failed: %d\n", rc);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr,
                "usage: %s <output.img> <wad-file> <name-on-volume> "
                "<base-lba>\n",
                argv[0]);
        return 1;
    }
    const char *img_path       = argv[1];
    const char *wad_path       = argv[2];
    const char *name_on_volume = argv[3];
    uint32_t    base_lba;
    if (parse_base_lba(argv[4], &base_lba) != 0) {
        fprintf(stderr, "mkexofsimg: invalid <base-lba> '%s'\n", argv[4]);
        return 1;
    }

    FILE *probe = fopen(wad_path, "rb");
    if (!probe) {
        perror(wad_path);
        return 1;
    }
    if (fseek(probe, 0, SEEK_END) != 0) {
        perror(wad_path);
        fclose(probe);
        return 1;
    }
    long wad_bytes = ftell(probe);
    fclose(probe);
    if (wad_bytes <= 0) {
        fprintf(stderr, "mkexofsimg: %s is empty or unreadable\n", wad_path);
        return 1;
    }

    uint32_t wad_sectors = (uint32_t)(((uint64_t)wad_bytes + 511u) / 512u);
    uint32_t vol_sectors = wad_sectors + SLACK_SECTORS;
    if (vol_sectors < EXOFS_MIN_SECTORS) {
        vol_sectors = EXOFS_MIN_SECTORS;
    }
    /* The whole file spans [0, base_lba + vol_sectors): everything below
     * base_lba is left zeroed, reserved scratch space for the fixtures
     * described above, not part of this volume. */
    uint32_t image_sectors = base_lba + vol_sectors;

    if (host_blockdev_open(img_path, image_sectors) != 0) {
        fprintf(stderr, "mkexofsimg: could not create %s\n", img_path);
        return 1;
    }

    int rc = exofs_format(base_lba, vol_sectors);
    if (rc != 0) {
        fprintf(stderr, "mkexofsimg: exofs_format failed: %d\n", rc);
        host_blockdev_close();
        remove(img_path);
        return 1;
    }

    rc = exofs_mount(base_lba);
    if (rc != 0) {
        fprintf(stderr, "mkexofsimg: exofs_mount failed: %d\n", rc);
        host_blockdev_close();
        remove(img_path);
        return 1;
    }

    char volume_path[256];
    snprintf(volume_path, sizeof(volume_path), "/%s", name_on_volume);

    if (write_file_into_volume(wad_path, volume_path) != 0) {
        exofs_unmount();
        host_blockdev_close();
        remove(img_path);
        return 1;
    }

    rc = exofs_sync();
    if (rc != 0) {
        fprintf(stderr, "mkexofsimg: exofs_sync failed: %d\n", rc);
        exofs_unmount();
        host_blockdev_close();
        remove(img_path);
        return 1;
    }

    exofs_unmount();
    host_blockdev_close();

    printf("mkexofsimg: wrote %ld bytes as %s into %s "
           "(volume at LBA %u, %u sectors; image %u sectors total)\n",
           wad_bytes, volume_path, img_path, base_lba, vol_sectors,
           image_sectors);
    return 0;
}
