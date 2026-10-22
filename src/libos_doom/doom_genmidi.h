#pragma once

#include <stdint.h>

#include "wad.h"

/*
 * doom_genmidi — the IWAD's GENMIDI OPL instrument table, decoded (SCRUM-216).
 *
 * A MIDI event from src/libos_doom/doom_midi.c says "program 48, note 60".
 * Turning that into OPL3 register writes needs an instrument definition, and
 * Doom ships one inside the IWAD: the GENMIDI lump, the table DMX's OPL
 * driver used.  This is the decoder for it and nothing else -- no timing, no
 * hardware, no voice allocation.  The OPL3 synthesizer (SCRUM-217) is the
 * consumer, and like it this runs in ring 3: it lives in src/libos_doom/ so
 * a shipped kernel does not carry it, and a TESTING kernel compiles it
 * explicitly (docker/scripts/build.sh step 3b3) so the doom_genmidi suite can
 * run it from ring 0.
 *
 * ── Layout ─────────────────────────────────────────────────────────────
 *
 *   0      8 bytes      "#OPL_II#"
 *   8      175 × 36     instrument records: 128 GM programs, then 47
 *                       percussion instruments for GM notes 35..81
 *   6308   175 × 32     instrument names, NUL-padded (not necessarily
 *                       NUL-terminated at exactly 32 characters)
 *   11908               end -- freedoom2 v0.13.0's lump is exactly this size
 *
 * Record (36 bytes, all multi-byte fields little-endian):
 *
 *   0  u16 flags        DOOM_GENMIDI_FLAG_*; bit 1 (0x0002, "delayed
 *                       vibrato") is unused by Doom and not decoded
 *   2  u8  fine_tune    second voice's detune, 128 = none
 *   3  u8  fixed_note   the note played when FLAG_FIXED is set
 *   4  voice[0]         16 bytes, below
 *   20 voice[1]         16 bytes; meaningful only with FLAG_2VOICE
 *
 * Voice (16 bytes):
 *
 *   0  modulator op     6 bytes, below
 *   6  u8  feedback     OPL register 0xC0: feedback << 1 | connection
 *   7  carrier op       6 bytes
 *   13 u8  (unused)
 *   14 s16 base_note_offset  semitones added to the played note
 *
 * Operator (6 bytes), each byte the OPL register it is written to:
 *
 *   0 tremolo   0x20  AM / vibrato / sustain (EG type) / KSR / multiplier
 *   1 attack    0x60  attack << 4 | decay
 *   2 sustain   0x80  sustain level << 4 | release
 *   3 waveform  0xE0  waveform select
 *   4 scale     0x40  key-scale level (top two bits)
 *   5 level     0x40  output level (bottom six); the synth ORs the two
 *
 * ── Zero copy, decoded by value ────────────────────────────────────────
 *
 * doom_genmidi_t points into the mounted IWAD, for the reason src/doom_dmx.h
 * gives: the IWAD is already identity-mapped and there is nowhere to copy it.
 * It must not outlive the mount.  Records, though, are returned decoded into
 * plain structs rather than overlaid: a lump starts at an arbitrary byte
 * offset, so base_note_offset is not guaranteed aligned for an int16_t load,
 * and a 30-byte copy per note-on costs nothing.
 *
 * ── The length check is exact ──────────────────────────────────────────
 *
 * The lump has a fixed layout with no count field, so any size other than
 * DOOM_GENMIDI_LUMP_BYTES is rejected rather than read.  chocolate-doom
 * accepts a larger lump and ignores the tail; this decoder does not, because
 * a GENMIDI of the wrong size is not a GENMIDI this decoder understands.
 */

#define DOOM_GENMIDI_MAGIC          "#OPL_II#"
#define DOOM_GENMIDI_HEADER_BYTES   8u
#define DOOM_GENMIDI_INSTR_BYTES    36u
#define DOOM_GENMIDI_VOICE_BYTES    16u
#define DOOM_GENMIDI_NAME_BYTES     32u
#define DOOM_GENMIDI_NUM_INSTRS     175u
#define DOOM_GENMIDI_NUM_PROGRAMS   128u
#define DOOM_GENMIDI_PERC_FIRST     35u   /* GM note of record 128 */
#define DOOM_GENMIDI_PERC_LAST      81u   /* GM note of record 174 */
#define DOOM_GENMIDI_NAMES_OFFSET \
    (DOOM_GENMIDI_HEADER_BYTES + DOOM_GENMIDI_NUM_INSTRS * DOOM_GENMIDI_INSTR_BYTES)
#define DOOM_GENMIDI_LUMP_BYTES \
    (DOOM_GENMIDI_NAMES_OFFSET + DOOM_GENMIDI_NUM_INSTRS * DOOM_GENMIDI_NAME_BYTES)

#define DOOM_GENMIDI_FLAG_FIXED     0x0001u  /* play fixed_note, ignore the event's note */
#define DOOM_GENMIDI_FLAG_2VOICE    0x0004u  /* voice[1] sounds too, detuned by fine_tune */

typedef struct {
    uint8_t tremolo;
    uint8_t attack;
    uint8_t sustain;
    uint8_t waveform;
    uint8_t scale;
    uint8_t level;
} doom_genmidi_op_t;

typedef struct {
    doom_genmidi_op_t mod;
    uint8_t           feedback;
    doom_genmidi_op_t car;
    int16_t           base_note_offset;
} doom_genmidi_voice_t;

typedef struct {
    uint16_t             flags;
    uint8_t              fine_tune;
    uint8_t              fixed_note;
    doom_genmidi_voice_t voice[2];
} doom_genmidi_instr_t;

typedef struct {
    const uint8_t *lump;    /* into the mounted WAD; DOOM_GENMIDI_LUMP_BYTES long */
} doom_genmidi_t;

/*
 * Result codes, distinct for the reason src/doom_dmx.h gives.  ENOENT is the
 * PWAD-only mount: not an error in the lump, just no instruments -- what to
 * do about that is the synth's decision, not this layer's.
 */
#define DOOM_GENMIDI_OK       0
#define DOOM_GENMIDI_ENOENT  -1  /* no GENMIDI lump in the directory */
#define DOOM_GENMIDI_EINVAL  -2  /* NULL, wrong size, or index/note out of range */
#define DOOM_GENMIDI_EFORMAT -3  /* magic is not DOOM_GENMIDI_MAGIC */

/*
 * Validate [lump, lump + size) as a GENMIDI lump.  The size is checked before
 * the magic, so nothing past `size` is ever read.  On every failure *out is
 * left untouched, as doom_dmx_parse() does.
 */
int doom_genmidi_parse(const void *lump, uint32_t size, doom_genmidi_t *out);

/* Find "GENMIDI" in `wad` and parse it.  DOOM_GENMIDI_ENOENT if absent. */
int doom_genmidi_find(const wad_t *wad, doom_genmidi_t *out);

/* Decode record `index` (0..174).  *out untouched on failure. */
int doom_genmidi_instr(const doom_genmidi_t *g, uint32_t index, doom_genmidi_instr_t *out);

/* The instrument for GM program 0..127 (record `program`). */
int doom_genmidi_program(const doom_genmidi_t *g, uint8_t program, doom_genmidi_instr_t *out);

/* The instrument for percussion-channel note 35..81 (record 128 + note - 35);
 * DOOM_GENMIDI_EINVAL for any other note. */
int doom_genmidi_percussion(const doom_genmidi_t *g, uint8_t note, doom_genmidi_instr_t *out);

/* Record `index`'s name, always NUL-terminated (a name may fill all 32
 * bytes).  "" for a NULL table or an out-of-range index. */
void doom_genmidi_name(const doom_genmidi_t *g, uint32_t index, char out[33]);

/* Human-readable form of a result code.  Never NULL. */
const char *doom_genmidi_strerror(int rc);
