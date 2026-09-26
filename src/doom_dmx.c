/*
 * doom_dmx.c — the DMX (`DS*`) sound lump decoder (SCRUM-211).
 *
 * See src/doom_dmx.h for what DMX is, why the result aliases the mounted WAD
 * instead of copying it, why the 16 padding samples at each end have to go,
 * and why the sample rate is reported rather than assumed.
 */

#include "doom_dmx.h"

#include "wad.h"

#include <stddef.h>
#include <stdint.h>

/*
 * Byte-at-a-time little-endian reads.
 *
 * A lump starts at an arbitrary byte offset in the WAD file (DSPISTOL is at
 * 9293940 in freedoom2, which is 4-byte aligned by luck and nothing else), so
 * the header fields are not guaranteed aligned for a uint16_t/uint32_t load.
 * src/wad.c gets away with __builtin_memcpy for the same reason this gets
 * away with shifts; shifts are used here so the endianness is stated in the
 * code rather than inherited from the target.
 */
static uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int doom_dmx_parse(const void *lump, uint32_t lump_size, doom_dmx_t *out)
{
    const uint8_t *bytes = lump;

    if (bytes == NULL || out == NULL) {
        return DOOM_DMX_EINVAL;
    }

    /*
     * Order here is the same discipline doom_wad_mount() follows: refuse
     * before reading anything the refusal would have made unsafe to read.
     * The header check comes first because every later field lives inside it.
     */
    if (lump_size < DOOM_DMX_HEADER_BYTES) {
        return DOOM_DMX_EINVAL;
    }

    uint16_t format  = rd_le16(bytes);
    uint16_t rate    = rd_le16(bytes + 2);
    uint32_t count   = rd_le32(bytes + 4);

    if (format != DOOM_DMX_FORMAT_PCM) {
        return DOOM_DMX_EFORMAT;
    }

    /*
     * Subtraction form, as src/wad.c does throughout: lump_size >= 8 is
     * already established above, so lump_size - DOOM_DMX_HEADER_BYTES cannot
     * wrap, while `count + 8 > lump_size` could.  A header claiming more
     * samples than the lump holds is the one malformation that would
     * otherwise walk off the end of a 28 MiB mapping.
     *
     * A count *smaller* than the body is accepted: all 109 of freedoom2's
     * DS lumps have count == lump_size - 8 exactly, but the header is the
     * authority on length and a few bytes of trailing slack is a padding
     * artefact, not a corruption.
     */
    if (count > lump_size - DOOM_DMX_HEADER_BYTES) {
        return DOOM_DMX_EINVAL;
    }

    /* Strictly greater: a lump that is *exactly* padding decodes to zero
     * samples, which no caller can do anything with and which would make
     * `samples` point one past the end of the padding it just skipped. */
    if (count <= 2u * DOOM_DMX_PAD_SAMPLES) {
        return DOOM_DMX_ETOOSHORT;
    }

    out->format      = format;
    out->rate_hz     = rate;
    out->samples     = bytes + DOOM_DMX_HEADER_BYTES + DOOM_DMX_PAD_SAMPLES;
    out->num_samples = count - 2u * DOOM_DMX_PAD_SAMPLES;

    return DOOM_DMX_OK;
}

int doom_dmx_find(const wad_t *wad, const char *lump_name, doom_dmx_t *out)
{
    if (wad == NULL || lump_name == NULL || out == NULL) {
        return DOOM_DMX_EINVAL;
    }

    uint32_t       lump_size = 0;
    const uint8_t *lump      = wad_find_lump(wad, lump_name, &lump_size);

    if (lump == NULL) {
        /* wad_find_lump() also returns NULL for a lump whose directory entry
         * points out of bounds, which is not really "absent" -- but it is
         * indistinguishable from here and equally unusable, and the WAD-level
         * bounds failure is wad_init()'s story to tell. */
        return DOOM_DMX_ENOENT;
    }

    return doom_dmx_parse(lump, lump_size, out);
}

void doom_dmx_lump_name(const char *sfx_name, char out[9])
{
    if (out == NULL) {
        return;
    }

    out[0] = 'D';
    out[1] = 'S';

    int n = 2;
    if (sfx_name != NULL) {
        for (; n < 8 && sfx_name[n - 2] != '\0'; n++) {
            char c = sfx_name[n - 2];
            /* Doom's sfxinfo_t::name entries are lower case ("pistol"); WAD
             * directory names are upper case, and wad.c's lump_name_eq() is
             * a byte compare with no case folding, so this fold is load
             * bearing rather than cosmetic. */
            if (c >= 'a' && c <= 'z') {
                c = (char)(c - 'a' + 'A');
            }
            out[n] = c;
        }
    }

    out[n] = '\0';
}

int doom_dmx_find_sfx(const wad_t *wad, const char *sfx_name, doom_dmx_t *out)
{
    if (sfx_name == NULL || sfx_name[0] == '\0') {
        /* An empty name would compose to the bare prefix "DS", which is a
         * legal 2-character lump name -- so this has to be rejected here
         * rather than left to the directory scan to not find. */
        return DOOM_DMX_EINVAL;
    }

    char lump_name[9];
    doom_dmx_lump_name(sfx_name, lump_name);

    return doom_dmx_find(wad, lump_name, out);
}

uint32_t doom_dmx_to_s16(const doom_dmx_t *dmx, int16_t *dst, uint32_t dst_samples)
{
    if (dmx == NULL || dmx->samples == NULL || dst == NULL) {
        return 0;
    }

    uint32_t n = dmx->num_samples;
    if (n > dst_samples) {
        n = dst_samples;
    }

    for (uint32_t i = 0; i < n; i++) {
        /* Via int, not a direct shift on the uint8_t: (u8 - 128) has to be
         * signed before it is scaled, or the low half of the range wraps. */
        dst[i] = (int16_t)(((int)dmx->samples[i] - 128) << 8);
    }

    return n;
}

const char *doom_dmx_strerror(int rc)
{
    switch (rc) {
    case DOOM_DMX_OK:
        return "ok";
    case DOOM_DMX_ENOENT:
        return "no such lump in the WAD directory";
    case DOOM_DMX_EINVAL:
        return "not a DMX lump (too short, or header length past the lump)";
    case DOOM_DMX_EFORMAT:
        return "wrong DMX format tag (not 8-bit PCM)";
    case DOOM_DMX_ETOOSHORT:
        return "no samples left after stripping DMX padding";
    default:
        return "unknown error";
    }
}
