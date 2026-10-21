/*
 * exofsfuse — mount an ExoFS volume from a disk image on the build host.
 *
 * build/exodoom_disk.img is not a FAT image despite step [1b/7]'s name: it
 * is an ExoFS volume (src/libos_fs/), and it does not start at LBA 0 --
 * LIBOS_WAD_DISK_BASE_LBA (src/doom_wad_disk.h) leaves the low sectors
 * zeroed for the raw-sector KUnit fixtures. Linux has no ExoFS driver, so
 * there is nothing for mount(8) to bind at any offset. This is that driver,
 * in user space.
 *
 * Like tools/mkexofsimg, it links the REAL src/libos_fs/ sources and
 * reaches the image through the third implementation of
 * exofs_blockdev.h's seam (tools/mkexofsimg/host_blockdev.c, extended here
 * with host_blockdev_open_existing() so an existing volume is attached
 * rather than overwritten). Nothing about the on-disk format is restated
 * here -- a format bug in the kernel's filesystem is a format bug in this
 * mount, which is the point: this is a window onto the same code, not a
 * second reader that could drift from it.
 *
 * SINGLE-THREADED, ALWAYS. exofs.h states it plainly: one static mounted
 * volume, no handle to pass around, no locking anywhere. main() therefore
 * appends "-s" to the FUSE argument list itself rather than documenting a
 * flag a caller could forget.
 *
 * Deliberately compiled WITHOUT "-I src", for the same reason mkexofsimg's
 * host-side files are: this file needs the real <stdio.h>/<stdlib.h>/
 * <string.h>, and "-I src" would shadow them with the project's
 * freestanding shims. It reaches exofs.h via "-I src/libos_fs" and
 * exo_errno.h by relative path -- that header is #pragma once plus
 * #defines, with no includes of its own, so naming it directly costs
 * nothing.
 *
 * ERRORS. EXO_E* values are deliberately the Linux errno numbers of the
 * same names (src/exo_errno.h's own header comment), so an ExoFS return
 * code is handed to FUSE unchanged -- there is no mapping table below and
 * there must not grow one.
 *
 * TIMESTAMPS are reported as epoch 0. ExoFS records exo_get_ticks() --
 * milliseconds since the boot that wrote the file (exofs_layout.h's "Time"
 * note) -- which is not a wall clock and is not comparable across mounts.
 * Showing 1970 says "no timestamp"; converting a tick count into a
 * plausible-looking date would not.
 */

#define FUSE_USE_VERSION 31

#include <fuse.h>

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "exofs.h"
#include "host_blockdev.h"

#include "../../src/exo_errno.h"

/* Default volume base: LIBOS_WAD_DISK_BASE_LBA (src/doom_wad_disk.h), the
 * volume build.sh step [1b/7] writes the IWAD into. Checked against that
 * header at build time by build.sh, the same way build.sh already checks
 * the IWAD name against LIBOS_WAD_FILE_NAME. */
#ifndef EXOFSFUSE_DEFAULT_BASE_LBA
#define EXOFSFUSE_DEFAULT_BASE_LBA 16384u
#endif

struct exofsfuse_opts {
    char *image;
    int   base_lba;
    int   read_only;
    int   show_help;
};

static struct exofsfuse_opts g_opts = {
    .image = NULL,
    .base_lba = (int)EXOFSFUSE_DEFAULT_BASE_LBA,
    .read_only = 0,
    .show_help = 0,
};

/* Set when the image itself could not be opened for writing (the Docker
 * build leaves build/exodoom_disk.img root-owned), as well as by -o ro.
 * Modifying calls are refused here rather than at exofs_bdev_write(): a
 * write that fails halfway through a structural operation is exactly the
 * partial state docs/filesystem.md §1's ordering rules exist to bound, and
 * there is no reason to walk into one when the answer is known up front. */
static int g_ro = 0;

#define EXOFSFUSE_OPT(t, p) { t, offsetof(struct exofsfuse_opts, p), 1 }

static const struct fuse_opt exofsfuse_opt_spec[] = {
    EXOFSFUSE_OPT("base_lba=%d", base_lba),
    EXOFSFUSE_OPT("ro", read_only),
    EXOFSFUSE_OPT("--help", show_help),
    EXOFSFUSE_OPT("-h", show_help),
    FUSE_OPT_END
};

static int opt_proc(void *data, const char *arg, int key,
                    struct fuse_args *outargs)
{
    struct exofsfuse_opts *o = data;
    (void)outargs;

    /* The first non-option argument is the image; everything after it
     * (the mountpoint) belongs to FUSE. */
    if (key == FUSE_OPT_KEY_NONOPT && o->image == NULL) {
        o->image = strdup(arg);
        return 0;
    }
    return 1;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
"usage: %s <image> <mountpoint> [options]\n"
"\n"
"Mount an ExoFS volume out of a raw disk image.\n"
"\n"
"ExoFS-specific options:\n"
"    -o base_lba=N   absolute LBA the volume starts at (default %u,\n"
"                    LIBOS_WAD_DISK_BASE_LBA -- the IWAD volume on\n"
"                    build/exodoom_disk.img)\n"
"    -o ro           mount read-only (also forced when the image file\n"
"                    itself is not writable)\n"
"\n"
"Useful FUSE options:\n"
"    -f              stay in the foreground\n"
"    -d              foreground, with protocol tracing\n"
"\n"
"Unmount with:  fusermount3 -u <mountpoint>\n",
            argv0, EXOFSFUSE_DEFAULT_BASE_LBA);
}

/* ---- helpers ------------------------------------------------------------ */

static int is_root(const char *path)
{
    return path[0] == '/' && path[1] == '\0';
}

static void fill_stat(struct stat *st, uint16_t attrs, uint32_t size)
{
    memset(st, 0, sizeof(*st));
    if (attrs & EXOFS_ATTR_DIRECTORY) {
        st->st_mode = S_IFDIR | 0755;
        /* 2 is the conventional "unknown subdirectory count" for a
         * filesystem that does not track one; ExoFS does not. */
        st->st_nlink = 2;
        st->st_size = EXOFS_BLOCK_SIZE;
    } else {
        st->st_mode = S_IFREG | 0644;
        st->st_nlink = 1;
        st->st_size = (off_t)size;
    }
    st->st_blksize = EXOFS_BLOCK_SIZE;
    st->st_blocks = (off_t)((st->st_size + 511) / 512);
    st->st_uid = getuid();
    st->st_gid = getgid();
    /* See the timestamp note at the top of this file. */
    st->st_atime = st->st_mtime = st->st_ctime = 0;
}

/* ---- FUSE operations ---------------------------------------------------- */

static void *op_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    (void)conn;
    /* Every path is resolved against the volume on every call, so there is
     * nothing for the kernel's attribute cache to go stale against that the
     * volume itself would not also have changed under. Keep the default
     * timeouts; what must be off is use_ino -- ExoFS has no inode number
     * and FUSE must invent one. */
    cfg->use_ino = 0;
    return NULL;
}

static int op_getattr(const char *path, struct stat *st,
                      struct fuse_file_info *fi)
{
    (void)fi;

    if (is_root(path)) {
        fill_stat(st, EXOFS_ATTR_DIRECTORY, 0);
        return 0;
    }

    exofs_stat_t s;
    int rc = exofs_stat(path, &s);
    if (rc < 0) {
        return rc;
    }
    fill_stat(st, s.attributes, s.size);
    return 0;
}

static int op_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                      off_t offset, struct fuse_file_info *fi,
                      enum fuse_readdir_flags flags)
{
    (void)offset;
    (void)fi;
    (void)flags;

    exofs_dir_t d;
    int rc = exofs_opendir(path, &d);
    if (rc < 0) {
        return rc;
    }

    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);

    exofs_dirinfo_t info;
    while ((rc = exofs_readdir(&d, &info)) == 1) {
        /* exofs_readdir reports "." and ".." as the ordinary entries they
         * are (exofs.h), and they are already filled in above. */
        if (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0) {
            continue;
        }
        struct stat st;
        fill_stat(&st, info.attributes, info.size);
        if (filler(buf, info.name, &st, 0, 0) != 0) {
            rc = -ENOMEM;
            break;
        }
    }
    exofs_closedir(&d);
    return rc < 0 ? rc : 0;
}

static int op_open(const char *path, struct fuse_file_info *fi)
{
    uint32_t flags = 0;
    int want_write = (fi->flags & (O_WRONLY | O_RDWR)) != 0;

    if (want_write && g_ro) {
        return -EROFS;
    }
    if ((fi->flags & O_ACCMODE) == O_RDONLY || (fi->flags & O_RDWR)) {
        flags |= EXOFS_O_READ;
    }
    if (want_write) {
        flags |= EXOFS_O_WRITE;
    }
    if (fi->flags & O_APPEND) {
        flags |= EXOFS_O_APPEND;
    }

    exofs_file_t *f = calloc(1, sizeof(*f));
    if (!f) {
        return -ENOMEM;
    }
    int rc = exofs_open(path, flags, f);
    if (rc < 0) {
        free(f);
        return rc;
    }
    fi->fh = (uint64_t)(uintptr_t)f;
    return 0;
}

static int op_create(const char *path, mode_t mode,
                     struct fuse_file_info *fi)
{
    (void)mode;   /* ExoFS has no permission bits to record. */

    if (g_ro) {
        return -EROFS;
    }

    exofs_file_t *f = calloc(1, sizeof(*f));
    if (!f) {
        return -ENOMEM;
    }
    int rc = exofs_open(path,
                        EXOFS_O_READ | EXOFS_O_WRITE | EXOFS_O_CREATE |
                        EXOFS_O_TRUNC, f);
    if (rc < 0) {
        free(f);
        return rc;
    }
    fi->fh = (uint64_t)(uintptr_t)f;
    return 0;
}

static int op_read(const char *path, char *buf, size_t size, off_t offset,
                   struct fuse_file_info *fi)
{
    (void)path;

    exofs_file_t *f = (exofs_file_t *)(uintptr_t)fi->fh;
    if (!f) {
        return -EBADF;
    }
    if (offset < 0) {
        return -EINVAL;
    }

    int64_t pos = exofs_seek(f, offset, EXOFS_SEEK_SET);
    if (pos < 0) {
        return (int)pos;
    }

    /* exofs_read takes a uint32_t count and reports an int64_t; FUSE asks
     * for size_t and wants an int. Chunk rather than truncate, so a single
     * oversized request still returns everything it asked for. */
    size_t done = 0;
    while (done < size) {
        uint32_t want = (size - done) > 0x100000u
                            ? 0x100000u
                            : (uint32_t)(size - done);
        int64_t got = exofs_read(f, buf + done, want);
        if (got < 0) {
            return done > 0 ? (int)done : (int)got;
        }
        if (got == 0) {
            break;      /* end of file */
        }
        done += (size_t)got;
    }
    return (int)done;
}

static int op_write(const char *path, const char *buf, size_t size,
                    off_t offset, struct fuse_file_info *fi)
{
    (void)path;

    if (g_ro) {
        return -EROFS;
    }

    exofs_file_t *f = (exofs_file_t *)(uintptr_t)fi->fh;
    if (!f) {
        return -EBADF;
    }
    if (offset < 0) {
        return -EINVAL;
    }

    /* An EXOFS_O_APPEND handle writes at the end regardless, so seeking
     * would be a lie rather than an error; skip it. */
    if (!(f->flags & EXOFS_O_APPEND)) {
        int64_t pos = exofs_seek(f, offset, EXOFS_SEEK_SET);
        if (pos < 0) {
            return (int)pos;
        }
    }

    size_t done = 0;
    while (done < size) {
        uint32_t want = (size - done) > 0x100000u
                            ? 0x100000u
                            : (uint32_t)(size - done);
        int64_t put = exofs_write(f, buf + done, want);
        if (put < 0) {
            return done > 0 ? (int)done : (int)put;
        }
        if (put == 0) {
            break;
        }
        done += (size_t)put;
    }
    return (int)done;
}

static int op_flush(const char *path, struct fuse_file_info *fi)
{
    (void)path;

    exofs_file_t *f = (exofs_file_t *)(uintptr_t)fi->fh;
    if (!f || !(f->flags & EXOFS_O_WRITE)) {
        return 0;
    }
    int rc = exofs_fflush(f);
    return rc < 0 ? rc : 0;
}

static int op_fsync(const char *path, int datasync,
                    struct fuse_file_info *fi)
{
    (void)datasync;
    return op_flush(path, fi);
}

static int op_release(const char *path, struct fuse_file_info *fi)
{
    (void)path;

    exofs_file_t *f = (exofs_file_t *)(uintptr_t)fi->fh;
    if (!f) {
        return 0;
    }
    /* A file's size and chain head live in its directory entry and are
     * written back here, not on every write (exofs_file_t's durability
     * note). The return value is dropped because FUSE discards release()
     * errors anyway -- op_flush above is where a failing writeback is
     * still reportable, and the kernel calls it first. */
    exofs_close(f);
    free(f);
    fi->fh = 0;
    return 0;
}

static int op_truncate(const char *path, off_t size,
                       struct fuse_file_info *fi)
{
    (void)fi;

    if (g_ro) {
        return -EROFS;
    }
    if (size < 0) {
        return -EINVAL;
    }
    if ((uint64_t)size > 0xFFFFFFFFull) {
        return -EFBIG;
    }

    if (size == 0) {
        /* The one truncation ExoFS implements directly. */
        exofs_file_t f;
        int rc = exofs_open(path, EXOFS_O_WRITE | EXOFS_O_TRUNC, &f);
        if (rc < 0) {
            return rc;
        }
        rc = exofs_close(&f);
        return rc < 0 ? rc : 0;
    }

    exofs_stat_t s;
    int rc = exofs_stat(path, &s);
    if (rc < 0) {
        return rc;
    }
    if (s.attributes & EXOFS_ATTR_DIRECTORY) {
        return -EISDIR;
    }
    if ((uint32_t)size == s.size) {
        return 0;
    }
    if ((uint32_t)size < s.size) {
        /* Shrinking to a non-zero length would mean walking the chain and
         * freeing its tail -- a FAT edit ExoFS exposes no call for, and
         * not one to open-code from outside the filesystem (see
         * docs/filesystem.md §1 on why chain teardown order is
         * load-bearing). Growing is expressible and is what a copy needs. */
        return -ENOSYS;
    }

    /* Grow: write one byte at the new last offset. Every block is zeroed
     * when allocated, so the gap reads as zeros (exofs.h, exofs_write). */
    exofs_file_t f;
    rc = exofs_open(path, EXOFS_O_WRITE, &f);
    if (rc < 0) {
        return rc;
    }
    int64_t pos = exofs_seek(&f, size - 1, EXOFS_SEEK_SET);
    if (pos < 0) {
        exofs_close(&f);
        return (int)pos;
    }
    const char zero = 0;
    int64_t put = exofs_write(&f, &zero, 1);
    if (put < 0) {
        exofs_close(&f);
        return (int)put;
    }
    rc = exofs_close(&f);
    return rc < 0 ? rc : 0;
}

static int op_unlink(const char *path)
{
    if (g_ro) {
        return -EROFS;
    }
    int rc = exofs_unlink(path);
    return rc < 0 ? rc : 0;
}

static int op_mkdir(const char *path, mode_t mode)
{
    (void)mode;

    if (g_ro) {
        return -EROFS;
    }
    int rc = exofs_mkdir(path);
    return rc < 0 ? rc : 0;
}

static int op_rmdir(const char *path)
{
    if (g_ro) {
        return -EROFS;
    }
    int rc = exofs_rmdir(path);
    return rc < 0 ? rc : 0;
}

static int op_utimens(const char *path, const struct timespec tv[2],
                      struct fuse_file_info *fi)
{
    (void)path;
    (void)tv;
    (void)fi;
    /* Accepted and discarded. ExoFS's timestamps are boot-relative tick
     * counts with no setter, and failing here would make an ordinary
     * `cp` report an error after copying the file correctly. */
    return g_ro ? -EROFS : 0;
}

static int op_chmod(const char *path, mode_t mode,
                    struct fuse_file_info *fi)
{
    (void)path;
    (void)mode;
    (void)fi;
    /* Same bargain as op_utimens: no permission bits on disk, and `cp -p`
     * should not fail over one. */
    return g_ro ? -EROFS : 0;
}

static int op_statfs(const char *path, struct statvfs *st)
{
    (void)path;

    exofs_geometry_t g;
    int rc = exofs_geometry(&g);
    if (rc < 0) {
        return rc;
    }

    memset(st, 0, sizeof(*st));
    st->f_bsize = EXOFS_BLOCK_SIZE;
    st->f_frsize = EXOFS_BLOCK_SIZE;
    st->f_blocks = g.total_blocks;
    /* ExoFS exposes no free-block count, and counting one here would mean
     * reading the FAT a second time behind the filesystem's back. df will
     * show the volume as full; that is a missing number, not a wrong one,
     * and nothing in this driver consults it. */
    st->f_bfree = 0;
    st->f_bavail = 0;
    st->f_namemax = EXOFS_MAX_NAME;
    return 0;
}

static const struct fuse_operations exofs_ops = {
    .init     = op_init,
    .getattr  = op_getattr,
    .readdir  = op_readdir,
    .open     = op_open,
    .create   = op_create,
    .read     = op_read,
    .write    = op_write,
    .flush    = op_flush,
    .fsync    = op_fsync,
    .release  = op_release,
    .truncate = op_truncate,
    .unlink   = op_unlink,
    .mkdir    = op_mkdir,
    .rmdir    = op_rmdir,
    .utimens  = op_utimens,
    .chmod    = op_chmod,
    .statfs   = op_statfs,
    /* No .rename: ExoFS has no rename, and emulating one as
     * copy-then-unlink would turn an atomic-looking call into a
     * non-atomic one that can lose the file. */
};

int main(int argc, char *argv[])
{
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

    if (fuse_opt_parse(&args, &g_opts, exofsfuse_opt_spec, opt_proc) != 0) {
        return 1;
    }
    if (g_opts.show_help) {
        usage(argv[0]);
        fuse_opt_free_args(&args);
        return 0;
    }
    if (!g_opts.image) {
        usage(argv[0]);
        fuse_opt_free_args(&args);
        return 1;
    }
    if (g_opts.base_lba < 0) {
        fprintf(stderr, "exofsfuse: base_lba must not be negative\n");
        fuse_opt_free_args(&args);
        return 1;
    }

    /* One static volume, no locking (exofs.h). Not negotiable, so it is
     * not left to the caller's command line. */
    fuse_opt_add_arg(&args, "-s");

    g_ro = g_opts.read_only;
    if (!g_ro && access(g_opts.image, W_OK) != 0) {
        fprintf(stderr,
                "exofsfuse: %s is not writable; mounting read-only\n",
                g_opts.image);
        g_ro = 1;
    }
    if (host_blockdev_open_existing(g_opts.image, g_ro) != 0) {
        fprintf(stderr,
                "exofsfuse: cannot open %s as a disk image "
                "(missing, unreadable, or not a whole number of "
                "512-byte sectors)\n",
                g_opts.image);
        fuse_opt_free_args(&args);
        return 1;
    }

    uint32_t sectors = host_blockdev_sectors();
    if ((uint32_t)g_opts.base_lba >= sectors) {
        fprintf(stderr,
                "exofsfuse: base_lba %d is past the end of %s "
                "(%u sectors)\n",
                g_opts.base_lba, g_opts.image, sectors);
        host_blockdev_close();
        fuse_opt_free_args(&args);
        return 1;
    }

    int rc = exofs_mount((uint32_t)g_opts.base_lba);
    if (rc < 0) {
        fprintf(stderr,
                "exofsfuse: no ExoFS volume at LBA %d of %s "
                "(exofs_mount: %d%s)\n",
                g_opts.base_lba, g_opts.image, rc,
                rc == -EXO_EINVAL ? " -- no superblock there" : "");
        host_blockdev_close();
        fuse_opt_free_args(&args);
        return 1;
    }

    fprintf(stderr, "exofsfuse: mounted %s @ LBA %d (%u sectors, %s)\n",
            g_opts.image, g_opts.base_lba, sectors,
            g_ro ? "read-only" : "read-write");

    int ret = fuse_main(args.argc, args.argv, &exofs_ops, NULL);

    exofs_unmount();
    host_blockdev_close();
    fuse_opt_free_args(&args);
    free(g_opts.image);
    return ret;
}
