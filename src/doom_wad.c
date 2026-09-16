/*
 * doom_wad.c — the mounted-IWAD registry behind DG_Init (SCRUM-73).
 *
 * See src/doom_wad.h for what this is and why it is not inside
 * src/doomgeneric_exo.c.
 */

#include "doom_wad.h"

#include "wad.h"

#include <stddef.h>
#include <stdint.h>

/*
 * One mount, not a table.
 *
 * Doom can layer PWADs over an IWAD (W_MergeFile), but it does that through
 * its own w_wad.c lump list, not by asking the platform for a second file --
 * so a second slot here would have no caller and would only invite someone to
 * assume a mount stack exists.  `mounted` is the flag rather than testing
 * `g_mounted.wad.data != NULL`, so a future WAD legitimately mapped at a
 * base this code has no business second-guessing cannot be mistaken for
 * "nothing mounted".
 */
static doom_wad_t g_mounted;
static int        mounted;

int doom_wad_mount(const void *data, uint32_t size)
{
    const uint8_t *bytes = data;
    doom_wad_t     candidate;

    /*
     * Order matters here, and it is cheapest-and-most-diagnostic first.
     *
     * The NULL/zero case is its own code because it is the one failure that
     * says nothing about the WAD at all: it means no multiboot module tag
     * reached us, so the thing to go and look at is src/grub.cfg's module2
     * line and build.sh's ISO staging, not the file. Before SCRUM-164 neither
     * of those existed and no module tag was ever produced, which is exactly
     * the shape of failure this distinguishes.
     */
    if (bytes == NULL || size == 0) {
        return DOOM_WAD_ENOENT;
    }

    /*
     * Checked before wad_init() rather than after.  libos_map_wad() refuses
     * to map past LIBOS_WAD_MAX_BYTES, so a size beyond that means the tail
     * of this range is NOT actually mapped -- and wad_init() would happily
     * read a directory offset out of a header sitting in mapped memory and
     * then bounds-check every lump against a `size` covering pages that
     * fault on touch.  The parse has to be refused before it starts, not
     * judged on its result.
     */
    if (size > DOOM_WAD_MAX_BYTES) {
        return DOOM_WAD_ETOOBIG;
    }

    /*
     * Parsed into a local first, committed only on success.  wad_init()
     * leaves *wad untouched on every failure path (src/wad.c), so writing
     * straight into g_mounted would also have been correct today -- but that
     * is a property of the current wad.c, not a promise its header makes, and
     * the caller of a failed mount is about to tear the LibOS down through
     * I_Error.  Anything reading the registry during that shutdown must see
     * "nothing mounted", not a partly-written one.
     */
    if (wad_init(&candidate.wad, bytes, size) != 0) {
        return DOOM_WAD_EINVAL;
    }

    candidate.size     = size;
    candidate.numlumps = candidate.wad.numlumps;

    /* wad_init() accepts both magics; only the first byte separates them,
     * and it has already been validated as 'I' or 'P'. */
    candidate.is_iwad = (bytes[0] == 'I');

    g_mounted = candidate;
    mounted   = 1;

    return DOOM_WAD_OK;
}

const doom_wad_t *doom_wad_mounted(void)
{
    return mounted ? &g_mounted : (const doom_wad_t *)0;
}

void doom_wad_unmount(void)
{
    mounted = 0;
}

const char *doom_wad_strerror(int rc)
{
    switch (rc) {
    case DOOM_WAD_OK:
        return "ok";
    case DOOM_WAD_ENOENT:
        return "no WAD module (empty or unmapped range)";
    case DOOM_WAD_ETOOBIG:
        return "WAD larger than the mapped window";
    case DOOM_WAD_EINVAL:
        return "not a WAD (bad magic or directory out of bounds)";
    default:
        return "unknown error";
    }
}
