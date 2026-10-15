#pragma once

#include <stdint.h>

/*
 * doom_wad_disk — the IWAD's ExoFS volume on the shared disk image
 * (SCRUM-190).
 *
 * build/exodoom_disk.img (docker/scripts/build.sh step [1b/7]) is the one
 * disk image every QEMU invocation attaches now, test and normal boot
 * alike -- src/ata.c only ever speaks to one drive, so there is exactly
 * one image to share. Its low LBAs keep the exact layout
 * build/ata_scratch.img used to have, so every existing raw-sector/ExoFS
 * KUnit fixture keeps working unchanged: test_ata_k.c's LBA 2048/2049,
 * test_syscall_disk_k.c's LBA 3072, test_exofs_k.c's
 * EXOFS_TEST_BASE_LBA=8192 through its own VOL_SECTORS=8192 (ending at
 * 16383). LIBOS_WAD_DISK_BASE_LBA is the first LBA past all of that.
 *
 * docker/scripts/build.sh's host-side image builder reads this same
 * #define (by sed, the way it already does for LIBOS_WAD_FILE_NAME in
 * src/libos_wad_params.h) so there is one number rather than two that have
 * to agree on where the volume starts.
 */
#define LIBOS_WAD_DISK_BASE_LBA 16384u

/*
 * Mount the ExoFS volume at LIBOS_WAD_DISK_BASE_LBA, read
 * LIBOS_WAD_FILE_NAME (src/libos_wad_params.h) into a freshly allocated
 * contiguous physical buffer, and cache it for the life of the kernel.
 *
 * This replaces the old mmap_find_module() lookup against a GRUB
 * multiboot module, which no longer exists (SCRUM-190) -- every caller
 * that used to pair mmap_find_module() with doom_wad_mount() now pairs
 * this with doom_wad_mount() instead. src/syscall_launch.c's real launch
 * path and every KUnit suite that wants the real IWAD's bytes
 * (test_doom_sound_pcm_k.c, test_hda_pcm_k.c, test_doom_dmx_k.c) share this
 * one cache rather than each re-reading ~28 MB off disk; only the first
 * caller, whichever it is, actually touches the disk.
 *
 * Fills in out_paddr/out_size and returns 0 on success, or leaves both
 * untouched and returns a negative EXO_E* on failure (no disk, no volume,
 * no file, a read error, or out of memory) -- which can only happen on the
 * first call, since every later one just copies the already-validated
 * cache and cannot fail.
 */
int64_t doom_wad_disk_get(uint64_t *out_paddr, uint64_t *out_size);
