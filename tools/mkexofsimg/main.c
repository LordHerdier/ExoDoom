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

/* Size of a host file in bytes, or -1 (after printing why). */
static long host_file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        perror(path);
        fclose(f);
        return -1;
    }
    long n = ftell(f);
    fclose(f);
    if (n <= 0) {
        fprintf(stderr, "mkexofsimg: %s is empty or unreadable\n", path);
        return -1;
    }
    return n;
}

static int write_file_into_volume(exofs_vol_t *vol, const char *host_path,
                                   const char *volume_path)
{
    FILE *in = fopen(host_path, "rb");
    if (!in) {
        perror(host_path);
        return -1;
    }

    exofs_file_t f;
    int rc = exofs_open(vol, volume_path,
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
    /* Optional extra files follow the four fixed arguments as
     * <host-file> <name-on-volume> pairs (SCRUM-230: program packages live
     * on the same volume as the IWAD until SCRUM-234/237 give programs
     * volumes of their own). */
    if (argc < 5 || ((argc - 5) % 2) != 0) {
        fprintf(stderr,
                "usage: %s <output.img> <wad-file> <name-on-volume> "
                "<base-lba> [<host-file> <name-on-volume>]...\n",
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

    long wad_bytes = host_file_size(wad_path);
    if (wad_bytes < 0) {
        return 1;
    }

    /* Each file is rounded up to whole sectors; the existing slack absorbs
     * the FAT and directory blocks as before. */
    uint64_t extra_sectors = 0;
    for (int i = 5; i < argc; i += 2) {
        long n = host_file_size(argv[i]);
        if (n < 0) {
            return 1;
        }
        extra_sectors += ((uint64_t)n + 511u) / 512u;
    }

    uint32_t wad_sectors = (uint32_t)(((uint64_t)wad_bytes + 511u) / 512u);
    uint32_t vol_sectors = wad_sectors + (uint32_t)extra_sectors + SLACK_SECTORS;
    if (vol_sectors < EXOFS_MIN_SECTORS) {
        vol_sectors = EXOFS_MIN_SECTORS;
    }
    /* MKEXOFSIMG_MIN_SECTORS: format at least this many sectors, so the
     * volume has room for files installed later (docker/scripts/build.sh
     * sets it when wads/ holds extra IWADs for `install`). Unset or empty
     * leaves the volume sized to its contents. */
    const char *min_env = getenv("MKEXOFSIMG_MIN_SECTORS");
    if (min_env && *min_env) {
        uint32_t min_sectors;
        if (parse_base_lba(min_env, &min_sectors) != 0) {
            fprintf(stderr, "mkexofsimg: invalid MKEXOFSIMG_MIN_SECTORS '%s'\n",
                    min_env);
            return 1;
        }
        if (vol_sectors < min_sectors) {
            vol_sectors = min_sectors;
        }
    }
    /* The whole file spans [0, base_lba + vol_sectors): everything below
     * base_lba is left zeroed, reserved scratch space for the fixtures
     * described above, not part of this volume. */
    uint32_t image_sectors = base_lba + vol_sectors;

    if (host_blockdev_open(img_path, image_sectors) != 0) {
        fprintf(stderr, "mkexofsimg: could not create %s\n", img_path);
        return 1;
    }

    int rc = exofs_format(0, base_lba, vol_sectors);
    if (rc != 0) {
        fprintf(stderr, "mkexofsimg: exofs_format failed: %d\n", rc);
        host_blockdev_close();
        remove(img_path);
        return 1;
    }

    exofs_vol_t *vol = NULL;
    rc = exofs_mount(0, base_lba, &vol);
    if (rc != 0) {
        fprintf(stderr, "mkexofsimg: exofs_mount failed: %d\n", rc);
        host_blockdev_close();
        remove(img_path);
        return 1;
    }

    char volume_path[256];
    snprintf(volume_path, sizeof(volume_path), "/%s", name_on_volume);

    if (write_file_into_volume(vol, wad_path, volume_path) != 0) {
        exofs_unmount(vol);
        host_blockdev_close();
        remove(img_path);
        return 1;
    }

    for (int i = 5; i < argc; i += 2) {
        char extra_path[256];
        int plen = snprintf(extra_path, sizeof(extra_path), "/%s", argv[i + 1]);
        if (plen < 0 || (size_t)plen >= sizeof(extra_path)) {
            fprintf(stderr, "mkexofsimg: name '%s' is too long\n", argv[i + 1]);
            exofs_unmount(vol);
            host_blockdev_close();
            remove(img_path);
            return 1;
        }
        if (write_file_into_volume(vol, argv[i], extra_path) != 0) {
            exofs_unmount(vol);
            host_blockdev_close();
            remove(img_path);
            return 1;
        }
        printf("mkexofsimg: wrote %s as %s\n", argv[i], extra_path);
    }

    rc = exofs_sync(vol);
    if (rc != 0) {
        fprintf(stderr, "mkexofsimg: exofs_sync failed: %d\n", rc);
        exofs_unmount(vol);
        host_blockdev_close();
        remove(img_path);
        return 1;
    }

    exofs_unmount(vol);
    host_blockdev_close();

    printf("mkexofsimg: wrote %ld bytes as %s into %s "
           "(volume at LBA %u, %u sectors; image %u sectors total)\n",
           wad_bytes, volume_path, img_path, base_lba, vol_sectors,
           image_sectors);
    return 0;
}
