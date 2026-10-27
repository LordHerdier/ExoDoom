#ifndef EXO_VOLUME_H
#define EXO_VOLUME_H

/*
 * exo_volume — where per-program filesystem volumes live on the one disk
 * (SCRUM-237).
 *
 * The shared disk image (build/exodoom_disk.img) is laid out, low to high:
 *
 *   [0, LIBOS_WAD_DISK_BASE_LBA)        KUnit fixtures + the shell's scratch
 *                                        volume (src/doom_wad_disk.h)
 *   [LIBOS_WAD_DISK_BASE_LBA,           the program volume: the IWAD and the
 *    EXO_VOLUME_REGION_BASE_LBA)         packages (docker/scripts/build.sh)
 *   [EXO_VOLUME_REGION_BASE_LBA, ...)   EXO_VOLUME_MAX_SLOTS slots of
 *                                        EXO_VOLUME_SLOT_SECTORS each, one
 *                                        per program that asks for a volume
 *
 * A package names a slot and a size in its manifest (src/exo_pkg.h,
 * volume_slot / volume_sectors). The kernel turns that into the sector extent
 * the program may bind with exo_disk_acquire (#26) and reach with
 * exo_disk_read/exo_disk_write; nothing else on the disk is reachable from
 * that program. Slot rather than free-form LBAs is what makes a volume
 * persistent and private across launches: a program cannot name another
 * program's sectors by asking for them, only by being packaged with its slot.
 *
 * Slots in use (a package picks its slot by hand, so this is the register of
 * who has which; two packages naming one slot cannot run at once):
 *
 *   0, 1   the two voldemo packages (SCRUM-237)
 *   2      Doom: savegames and config (SCRUM-267)
 *
 * Header-only and <stdint.h>-only, like src/exo_pkg.h, because the host-side
 * packer (tools/exopkg) includes it.
 */

#include <stdint.h>

/* 128 MiB in: well past the program volume (the IWAD is ~28 MB from LBA
 * 16384, ending near LBA 75000) with room for it to grow. */
#define EXO_VOLUME_REGION_BASE_LBA 262144u

#define EXO_VOLUME_SLOT_SECTORS    8192u   /* 4 MiB per slot                 */
#define EXO_VOLUME_MAX_SLOTS       8u

#define EXO_VOLUME_SLOT_DOOM       2u      /* docker/scripts/build.sh's
                                            * doom.exo --volume-slot */

/* First sector past the last slot: the disk must be at least this big for
 * every slot to exist. */
#define EXO_VOLUME_REGION_END_LBA \
    (EXO_VOLUME_REGION_BASE_LBA + EXO_VOLUME_MAX_SLOTS * EXO_VOLUME_SLOT_SECTORS)

/* Base LBA of `slot`; the caller has checked slot < EXO_VOLUME_MAX_SLOTS. */
static inline uint32_t exo_volume_slot_base(uint32_t slot)
{
    return EXO_VOLUME_REGION_BASE_LBA + slot * EXO_VOLUME_SLOT_SECTORS;
}

#endif /* EXO_VOLUME_H */
