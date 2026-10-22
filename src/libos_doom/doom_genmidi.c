/*
 * doom_genmidi.c — the GENMIDI OPL instrument table decoder (SCRUM-216).
 *
 * See src/libos_doom/doom_genmidi.h for the lump layout, which OPL register
 * each operator byte feeds, and why records are decoded by value while the
 * table itself stays in the mounted WAD.
 */

#include <stddef.h>
#include <stdint.h>

#include "doom_genmidi.h"

#include "wad.h"

/* Byte-wise little-endian read; a lump's offset in the WAD is arbitrary, so
 * nothing in it is guaranteed aligned (same reasoning as src/doom_dmx.c). */
static uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void decode_op(const uint8_t *p, doom_genmidi_op_t *op)
{
    op->tremolo  = p[0];
    op->attack   = p[1];
    op->sustain  = p[2];
    op->waveform = p[3];
    op->scale    = p[4];
    op->level    = p[5];
}

static void decode_voice(const uint8_t *p, doom_genmidi_voice_t *v)
{
    decode_op(p, &v->mod);
    v->feedback = p[6];
    decode_op(p + 7, &v->car);
    /* p[13] is unused padding. */
    v->base_note_offset = (int16_t)rd_le16(p + 14);
}

int doom_genmidi_parse(const void *lump, uint32_t size, doom_genmidi_t *out)
{
    const uint8_t *bytes = lump;
    static const char magic[] = DOOM_GENMIDI_MAGIC;

    if (bytes == NULL || out == NULL) {
        return DOOM_GENMIDI_EINVAL;
    }
    /* Size before magic: a lump shorter than its header must not be read. */
    if (size != DOOM_GENMIDI_LUMP_BYTES) {
        return DOOM_GENMIDI_EINVAL;
    }
    for (uint32_t i = 0; i < DOOM_GENMIDI_HEADER_BYTES; i++) {
        if (bytes[i] != (uint8_t)magic[i]) {
            return DOOM_GENMIDI_EFORMAT;
        }
    }

    out->lump = bytes;
    return DOOM_GENMIDI_OK;
}

int doom_genmidi_find(const wad_t *wad, doom_genmidi_t *out)
{
    uint32_t size = 0;

    if (wad == NULL || out == NULL) {
        return DOOM_GENMIDI_EINVAL;
    }
    const uint8_t *lump = wad_find_lump(wad, "GENMIDI", &size);
    if (lump == NULL) {
        return DOOM_GENMIDI_ENOENT;
    }
    return doom_genmidi_parse(lump, size, out);
}

int doom_genmidi_instr(const doom_genmidi_t *g, uint32_t index, doom_genmidi_instr_t *out)
{
    if (g == NULL || g->lump == NULL || out == NULL ||
        index >= DOOM_GENMIDI_NUM_INSTRS) {
        return DOOM_GENMIDI_EINVAL;
    }

    const uint8_t *r = g->lump + DOOM_GENMIDI_HEADER_BYTES +
                       index * DOOM_GENMIDI_INSTR_BYTES;

    out->flags      = rd_le16(r);
    out->fine_tune  = r[2];
    out->fixed_note = r[3];
    decode_voice(r + 4, &out->voice[0]);
    decode_voice(r + 4 + DOOM_GENMIDI_VOICE_BYTES, &out->voice[1]);
    return DOOM_GENMIDI_OK;
}

int doom_genmidi_program(const doom_genmidi_t *g, uint8_t program, doom_genmidi_instr_t *out)
{
    if (program >= DOOM_GENMIDI_NUM_PROGRAMS) {
        return DOOM_GENMIDI_EINVAL;
    }
    return doom_genmidi_instr(g, program, out);
}

int doom_genmidi_percussion(const doom_genmidi_t *g, uint8_t note, doom_genmidi_instr_t *out)
{
    if (note < DOOM_GENMIDI_PERC_FIRST || note > DOOM_GENMIDI_PERC_LAST) {
        return DOOM_GENMIDI_EINVAL;
    }
    return doom_genmidi_instr(g, DOOM_GENMIDI_NUM_PROGRAMS +
                                 (uint32_t)(note - DOOM_GENMIDI_PERC_FIRST), out);
}

void doom_genmidi_name(const doom_genmidi_t *g, uint32_t index, char out[33])
{
    uint32_t n = 0;

    if (out == NULL) {
        return;
    }
    if (g != NULL && g->lump != NULL && index < DOOM_GENMIDI_NUM_INSTRS) {
        const uint8_t *p = g->lump + DOOM_GENMIDI_NAMES_OFFSET +
                           index * DOOM_GENMIDI_NAME_BYTES;
        while (n < DOOM_GENMIDI_NAME_BYTES && p[n] != 0) {
            out[n] = (char)p[n];
            n++;
        }
    }
    out[n] = '\0';
}

const char *doom_genmidi_strerror(int rc)
{
    switch (rc) {
    case DOOM_GENMIDI_OK:      return "ok";
    case DOOM_GENMIDI_ENOENT:  return "no GENMIDI lump";
    case DOOM_GENMIDI_EINVAL:  return "invalid argument or lump size";
    case DOOM_GENMIDI_EFORMAT: return "bad GENMIDI magic";
    default:                   return "unknown GENMIDI error";
    }
}
