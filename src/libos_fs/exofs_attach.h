#ifndef EXOFS_ATTACH_H
#define EXOFS_ATTACH_H

/*
 * exofs_attach — mount a program's own volume, formatting it only if it has
 * never been formatted (SCRUM-267).
 *
 * A program with a private volume (src/exo_volume.h) finds either a volume it
 * made on an earlier run, or a slot that nothing has ever written. The first
 * is mounted; the second needs exofs_format(). The two have to be told apart
 * by what is on the disk, not by whether the mount worked: exofs_mount()
 * answers -EXO_EINVAL for a blank slot and for a damaged volume alike, and
 * formatting the second to "fix" it would erase a player's savegames because
 * of a bad sector. So the decision is made on the first sector (the
 * superblock's): all zero is blank, anything else is somebody's data and is
 * never formatted over.
 *
 * Lives in src/libos_fs/, so it knows only exofs_* and the LibOS heap. It does
 * not acquire the disk: the caller binds the extent first (exo_disk_acquire),
 * because that is a syscall and this layer makes none of its own.
 */

#include "exofs.h"

#include <stddef.h>
#include <stdint.h>

/* exofs_attach() results >= 0. */
#define EXOFS_ATTACH_MOUNTED   0   /* an existing volume was mounted       */
#define EXOFS_ATTACH_FORMATTED 1   /* the slot was blank: formatted, mounted */

/* Whether all `len` bytes of `sector` are zero -- what a slot nobody has
 * written looks like. Pure, so the decision is testable without a disk. */
int exofs_sector_is_blank(const void *sector, size_t len);

/*
 * Mount the volume at `base_lba` of `dev` into *out, formatting it first when
 * its first sector is blank. `sectors` is the extent's length, used only for
 * the format. Returns EXOFS_ATTACH_MOUNTED or EXOFS_ATTACH_FORMATTED, or a
 * negative -EXO_E* code: the mount's own for a volume that is present but
 * will not mount (left untouched), -EXO_EBUSY for one already mounted, a
 * device error from the probe, or the format's.
 */
int exofs_attach(uint32_t dev, uint32_t base_lba, uint32_t sectors,
                 exofs_vol_t **out);

#endif /* EXOFS_ATTACH_H */
