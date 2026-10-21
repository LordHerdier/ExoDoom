#!/usr/bin/env bash
#
# build.sh -- build build/exofsfuse, the host-side ExoFS FUSE driver.
#
# Runs on the BUILD HOST with the native compiler, not in the kernel's
# Docker image and not with the x86_64-elf cross toolchain: the output is a
# Linux user-space program that mounts an image on this machine. That is the
# same arrangement tools/mkexofsimg already has inside docker/scripts/
# build.sh step [1b/7] -- this is its counterpart, and it reuses that tool's
# host_blockdev.c/host_libos_heap.c rather than carrying a second copy of
# exofs_blockdev.h's seam.
#
# The src/libos_fs/*.c objects are compiled with "-I src" and the host-side
# ones without it, exactly as step [1b/7] does: the filesystem sources want
# the project's freestanding headers, the host shims want the real libc
# ones, and "-I src" is what decides which <stdio.h> a file sees.
#
# Needs fuse3's development files. On a distro: libfuse3-dev / fuse3-devel.
# On NixOS, where there is no system pkg-config path:
#   nix shell nixpkgs#fuse3.dev nixpkgs#pkg-config --command tools/exofsfuse/build.sh
set -euo pipefail

cd "$(dirname "$0")/../.."

CC="${CC:-gcc}"
# OUTDIR defaults to build/, next to every other build product. On a host
# where the Docker builds have already run, build/ belongs to root and is
# not writable by the invoking user -- point OUTDIR somewhere writable
# rather than reaching for sudo to drop a host-side tool into it.
OUTDIR="${OUTDIR:-build}"
OUT="$OUTDIR/exofsfuse"
OBJ="$OUTDIR/exofsfuse_obj"

if ! command -v pkg-config >/dev/null 2>&1; then
  echo "ERROR: pkg-config not found; needed to locate fuse3" >&2
  exit 1
fi
if ! pkg-config --exists fuse3; then
  echo "ERROR: fuse3 development files not found (pkg-config fuse3)." >&2
  echo "       Install libfuse3-dev / fuse3-devel, or on NixOS run:" >&2
  echo "       nix shell nixpkgs#fuse3.dev nixpkgs#pkg-config \\" >&2
  echo "         --command tools/exofsfuse/build.sh" >&2
  exit 1
fi

FUSE_CFLAGS="$(pkg-config --cflags fuse3)"
FUSE_LIBS="$(pkg-config --libs fuse3)"

# The default base LBA this driver mounts at is LIBOS_WAD_DISK_BASE_LBA, and
# it is read from the header rather than restated, the same way step [1b/7]
# reads it for mkexofsimg. A driver whose default silently disagreed with
# the volume the build writes would fail with "no superblock there" and give
# no hint why.
base_lba="$(sed -n 's/^#define LIBOS_WAD_DISK_BASE_LBA \([0-9]*\)u\?$/\1/p' \
            src/doom_wad_disk.h)"
if [[ -z "$base_lba" ]]; then
  echo "ERROR: could not read LIBOS_WAD_DISK_BASE_LBA from src/doom_wad_disk.h" >&2
  exit 1
fi

WARN=(-std=c99 -O2 -Wall -Wextra -Werror)
# main.c alone is gnu99 + _GNU_SOURCE: it is the only file here that talks
# to POSIX (strdup, S_IFDIR, struct timespec) and to libfuse, whose own
# header uses them too. _FILE_OFFSET_BITS=64 is libfuse3's documented
# requirement -- off_t appears in the operations struct, so a mismatch
# would be an ABI mismatch, not a warning.
FUSE_MAIN_FLAGS=(-std=gnu99 -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64
                 -O2 -Wall -Wextra -Werror)

if ! mkdir -p "$OBJ" 2>/dev/null; then
  echo "ERROR: cannot write to $OUTDIR (root-owned after a Docker build?)." >&2
  echo "       Re-run with OUTDIR=<a writable directory>." >&2
  exit 1
fi

echo "[1/3] host-side objects (no -I src)"
# shellcheck disable=SC2086
$CC "${FUSE_MAIN_FLAGS[@]}" $FUSE_CFLAGS -I src/libos_fs -I tools/mkexofsimg \
  -DEXOFSFUSE_DEFAULT_BASE_LBA="${base_lba}u" \
  -c tools/exofsfuse/main.c -o "$OBJ/main.o"
$CC "${WARN[@]}" -I src/libos_fs \
  -c tools/mkexofsimg/host_blockdev.c -o "$OBJ/host_blockdev.o"
$CC "${WARN[@]}" \
  -c tools/mkexofsimg/host_libos_heap.c -o "$OBJ/host_libos_heap.o"

echo "[2/3] src/libos_fs objects (-I src)"
for f in exofs_volume exofs_fat exofs_dir exofs_dirent exofs_name exofs_path \
         exofs_file; do
  $CC "${WARN[@]}" -I src -c "src/libos_fs/$f.c" -o "$OBJ/$f.o"
done

echo "[3/3] link $OUT"
# shellcheck disable=SC2086
$CC -o "$OUT" "$OBJ"/*.o $FUSE_LIBS

echo "built $OUT (default base_lba=$base_lba)"
