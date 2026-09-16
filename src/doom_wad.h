#pragma once

#include <stdint.h>

#include "wad.h"

/*
 * doom_wad — the mounted IWAD, as DG_Init leaves it (SCRUM-73).
 *
 * v1 has no filesystem.  freedoom2.wad arrives as a GRUB multiboot 2 module
 * (tag type 3, src/grub.cfg's `module2` line), the kernel identity-maps it at
 * vmm_init() and page_alloc_init() reserves its pages as PAGE_OWNER_KERNEL,
 * and a launched LibOS gets it read-only at LIBOS_WAD_VADDR via
 * libos_map_wad() (src/libos_wad_map.h) rather than by opening anything.
 *
 * So "loading the IWAD" is not a read into a buffer -- the bytes are already
 * mapped and there is nowhere to copy 28 MB to.  What DG_Init actually has to
 * do is prove the mapped region really is a WAD and record where it is, which
 * is what this registry holds.
 *
 * ── Why this is its own file ───────────────────────────────────────────
 *
 * Keeping it out of src/doomgeneric_exo.c is what makes it testable: this
 * has no DG_* signature to satisfy, takes a plain pointer rather than
 * reaching for a global, and returns an error code instead of halting.
 * tests/kernel/test_doom_wad_k.c drives every rejection path against
 * synthetic WAD headers from ring 0, which is not something a
 * `void DG_Init()` could ever be asked to do.
 */

/* The mount is rejected past this size.  Matches LIBOS_WAD_MAX_BYTES
 * (src/libos_wad_map.h), the ceiling libos_map_wad() itself refuses to map
 * past -- restated rather than included because that header is kernel-only
 * and this file is compiled for the ring-3 side too.  freedoom2.wad is
 * ~28 MiB, so this is generous headroom rather than a tight fit. */
#define DOOM_WAD_MAX_BYTES (64u * 1024u * 1024u)

typedef struct {
    wad_t    wad;       /* the parsed directory; see src/wad.h */
    uint32_t size;      /* byte length as mounted */
    uint32_t numlumps;  /* mirrored out of `wad` for cheap reporting */
    int      is_iwad;   /* 1 for IWAD, 0 for PWAD */
} doom_wad_t;

/* doom_wad_mount() result codes.  Distinct values rather than a bare -1
 * because each one means a different thing went wrong upstream, and DG_Init
 * reports them by name on serial -- "not a WAD" and "the module tag was
 * empty" send an investigation in completely different directions. */
#define DOOM_WAD_OK        0
#define DOOM_WAD_ENOENT   -1 /* NULL base, or zero length: no module reached us */
#define DOOM_WAD_ETOOBIG  -2 /* larger than DOOM_WAD_MAX_BYTES */
#define DOOM_WAD_EINVAL   -3 /* wad_init() refused it: bad magic or directory */

/*
 * Validate [data, data + size) as a WAD and record it as the mounted one.
 *
 * Returns DOOM_WAD_OK, or one of the codes above.  On any failure nothing is
 * recorded, so doom_wad_mounted() keeps answering NULL rather than handing
 * back a half-initialised parse -- which matters because the caller of a
 * failed mount is about to I_Error, and anything still reading the registry
 * during that shutdown must not see a WAD that was never there.
 *
 * A PWAD is accepted and flagged rather than refused.  Doom itself decides
 * whether it has a usable game (d_iwad.c), and refusing here would turn a
 * "this is a patch WAD, not a base game" problem into a "DG_Init rejected
 * your file" one -- which is the failure docs/architecture.md sec7 records
 * having already cost a debugging session once, when a PWAD shipped as the
 * IWAD and surfaced deep inside W_Init instead of at the four bytes that
 * actually said so.
 */
int doom_wad_mount(const void *data, uint32_t size);

/* The mounted WAD, or NULL if nothing has been mounted (or the last mount
 * failed).  The returned pointer is to static storage, valid for the life of
 * the process; the WAD bytes it points into are the kernel-mapped module, so
 * they are read-only and outlive any LibOS. */
const doom_wad_t *doom_wad_mounted(void);

/* Forget the mounted WAD.  Exists for tests, which mount a series of
 * deliberately malformed headers and must not leave one behind. */
void doom_wad_unmount(void);

/* Human-readable form of a doom_wad_mount() result, for the serial report.
 * Never NULL, including for an unrecognised code. */
const char *doom_wad_strerror(int rc);
