/*
 * test_doom_genmidi_k.c — SCRUM-216's acceptance tests for the GENMIDI OPL
 * instrument table decoder.
 *
 * Same two halves as test_doom_dmx_k.c.  The synthetic half builds a lump by
 * hand, because the inputs that must be rejected (a flipped magic byte, a
 * lump one byte short) are not files to point at, and because an exact
 * byte-to-field mapping is only assertable against bytes the test wrote.  The
 * real half decodes the mounted freedoom2.wad's own GENMIDI and asserts known
 * records field by field, so a decoder that is merely self-consistent with
 * the synthetic builder still has to agree with the table Doom ships.
 */

#include "kunit.h"

#include "libos_doom/doom_genmidi.h"
#include "doom_wad.h"
#include "doom_wad_disk.h"
#include "string.h"
#include "wad.h"

#include <stddef.h>
#include <stdint.h>

/* ---- helpers ------------------------------------------------------------ */

static void check_op(const doom_genmidi_op_t *op, const uint8_t want[6])
{
    CU_ASSERT_EQUAL(op->tremolo,  want[0]);
    CU_ASSERT_EQUAL(op->attack,   want[1]);
    CU_ASSERT_EQUAL(op->sustain,  want[2]);
    CU_ASSERT_EQUAL(op->waveform, want[3]);
    CU_ASSERT_EQUAL(op->scale,    want[4]);
    CU_ASSERT_EQUAL(op->level,    want[5]);
}

static void check_voice(const doom_genmidi_voice_t *v, const uint8_t mod[6],
                        uint8_t feedback, const uint8_t car[6], int16_t offset)
{
    check_op(&v->mod, mod);
    CU_ASSERT_EQUAL(v->feedback, feedback);
    check_op(&v->car, car);
    CU_ASSERT_EQUAL(v->base_note_offset, offset);
}

static int op_eq(const doom_genmidi_op_t *a, const doom_genmidi_op_t *b)
{
    return a->tremolo == b->tremolo && a->attack == b->attack &&
           a->sustain == b->sustain && a->waveform == b->waveform &&
           a->scale == b->scale && a->level == b->level;
}

static int instr_eq(const doom_genmidi_instr_t *a, const doom_genmidi_instr_t *b)
{
    if (a->flags != b->flags || a->fine_tune != b->fine_tune ||
        a->fixed_note != b->fixed_note) {
        return 0;
    }
    for (int k = 0; k < 2; k++) {
        const doom_genmidi_voice_t *va = &a->voice[k], *vb = &b->voice[k];
        if (!op_eq(&va->mod, &vb->mod) || !op_eq(&va->car, &vb->car) ||
            va->feedback != vb->feedback ||
            va->base_note_offset != vb->base_note_offset) {
            return 0;
        }
    }
    return 1;
}

/* The decoder never writes a record field it did not read, so a sentinel
 * that survives a call proves the call left *out alone. */
static void fill_sentinel(doom_genmidi_instr_t *in)
{
    memset(in, 0xA5, sizeof(*in));
}

static int is_sentinel(const doom_genmidi_instr_t *in)
{
    const uint8_t *p = (const uint8_t *)in;
    for (size_t i = 0; i < sizeof(*in); i++) {
        if (p[i] != 0xA5) {
            return 0;
        }
    }
    return 1;
}

/* ---- a synthetic lump --------------------------------------------------- */

/*
 * One byte of slack in front so the lump starts at an odd address: a decoder
 * that read base_note_offset with an int16_t load would still pass on x86,
 * but one that read it from the wrong offset would not, and the odd start
 * keeps the test honest about not depending on alignment.
 */
static uint8_t raw[1u + DOOM_GENMIDI_LUMP_BYTES + 16u];
#define SYN (raw + 1)

static void syn_build(void)
{
    memset(raw, 0, sizeof(raw));
    memcpy(SYN, DOOM_GENMIDI_MAGIC, DOOM_GENMIDI_HEADER_BYTES);
}

static uint8_t *syn_record(uint32_t i)
{
    return SYN + DOOM_GENMIDI_HEADER_BYTES + i * DOOM_GENMIDI_INSTR_BYTES;
}

static void test_layout_constants(void)
{
    CU_ASSERT_EQUAL(DOOM_GENMIDI_LUMP_BYTES, 11908u);
    CU_ASSERT_EQUAL(DOOM_GENMIDI_NAMES_OFFSET, 6308u);
    CU_ASSERT_EQUAL(DOOM_GENMIDI_NUM_PROGRAMS +
                    (DOOM_GENMIDI_PERC_LAST - DOOM_GENMIDI_PERC_FIRST + 1u),
                    DOOM_GENMIDI_NUM_INSTRS);
}

static void test_synthetic_record_decodes_exactly(void)
{
    doom_genmidi_t g;
    doom_genmidi_instr_t in;

    syn_build();
    uint8_t *r = syn_record(77);
    const uint8_t rec[36] = {
        0x05, 0x00,                     /* flags: FIXED | 2VOICE */
        0x7F, 0x2A,                     /* fine_tune, fixed_note */
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06,   0x07,
        0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D,   0xEE, 0xF4, 0xFF,   /* -12 */
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16,   0x17,
        0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D,   0xEE, 0x34, 0x12,   /* 0x1234 */
    };
    memcpy(r, rec, sizeof(rec));

    CU_ASSERT_EQUAL(doom_genmidi_parse(SYN, DOOM_GENMIDI_LUMP_BYTES, &g), DOOM_GENMIDI_OK);
    CU_ASSERT((g.lump) == (SYN));
    CU_ASSERT_EQUAL(doom_genmidi_instr(&g, 77, &in), DOOM_GENMIDI_OK);

    CU_ASSERT_EQUAL(in.flags, DOOM_GENMIDI_FLAG_FIXED | DOOM_GENMIDI_FLAG_2VOICE);
    CU_ASSERT_EQUAL(in.fine_tune, 0x7F);
    CU_ASSERT_EQUAL(in.fixed_note, 0x2A);
    check_voice(&in.voice[0], rec + 4, 0x07, rec + 11, -12);
    check_voice(&in.voice[1], rec + 20, 0x17, rec + 27, 0x1234);

    /* Its neighbours are untouched zeros. */
    CU_ASSERT_EQUAL(doom_genmidi_instr(&g, 76, &in), DOOM_GENMIDI_OK);
    CU_ASSERT_EQUAL(in.flags, 0);
    CU_ASSERT_EQUAL(in.voice[1].base_note_offset, 0);
    CU_ASSERT_EQUAL(doom_genmidi_instr(&g, 78, &in), DOOM_GENMIDI_OK);
    CU_ASSERT_EQUAL(in.voice[0].mod.tremolo, 0);
}

static void test_bad_magic_is_eformat(void)
{
    doom_genmidi_t g = { .lump = (const uint8_t *)0x1 };

    for (uint32_t i = 0; i < DOOM_GENMIDI_HEADER_BYTES; i++) {
        syn_build();
        SYN[i] ^= 0x20;
        CU_ASSERT_EQUAL(doom_genmidi_parse(SYN, DOOM_GENMIDI_LUMP_BYTES, &g),
                        DOOM_GENMIDI_EFORMAT);
    }
    CU_ASSERT((g.lump) == ((const uint8_t *)0x1));
}

static void test_wrong_size_is_einval(void)
{
    doom_genmidi_t g = { .lump = (const uint8_t *)0x1 };
    const uint32_t sizes[] = {
        0, 7, DOOM_GENMIDI_HEADER_BYTES,
        DOOM_GENMIDI_LUMP_BYTES - 1u, DOOM_GENMIDI_LUMP_BYTES + 1u,
        DOOM_GENMIDI_LUMP_BYTES + 32u,
    };

    /* A valid magic in front, so only the size can be what refuses it. */
    syn_build();
    for (uint32_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        CU_ASSERT_EQUAL(doom_genmidi_parse(SYN, sizes[i], &g), DOOM_GENMIDI_EINVAL);
    }
    CU_ASSERT((g.lump) == ((const uint8_t *)0x1));

    /*
     * Size before magic: a 4-byte "lump" sitting at the very end of the
     * buffer, followed by bytes that are not magic.  If the magic were read
     * first this would be EFORMAT; EINVAL says it was refused unread.
     */
    uint8_t *tail = raw + sizeof(raw) - 4u;
    memcpy(tail, "#OPL", 4);
    CU_ASSERT_EQUAL(doom_genmidi_parse(tail, 4, &g), DOOM_GENMIDI_EINVAL);
}

static void test_null_args_are_einval(void)
{
    doom_genmidi_t g;
    doom_genmidi_instr_t in;

    syn_build();
    CU_ASSERT_EQUAL(doom_genmidi_parse(NULL, DOOM_GENMIDI_LUMP_BYTES, &g), DOOM_GENMIDI_EINVAL);
    CU_ASSERT_EQUAL(doom_genmidi_parse(SYN, DOOM_GENMIDI_LUMP_BYTES, NULL), DOOM_GENMIDI_EINVAL);
    CU_ASSERT_EQUAL(doom_genmidi_find(NULL, &g), DOOM_GENMIDI_EINVAL);
    CU_ASSERT_EQUAL(doom_genmidi_instr(NULL, 0, &in), DOOM_GENMIDI_EINVAL);

    doom_genmidi_t empty = { .lump = NULL };
    CU_ASSERT_EQUAL(doom_genmidi_instr(&empty, 0, &in), DOOM_GENMIDI_EINVAL);

    CU_ASSERT_EQUAL(doom_genmidi_parse(SYN, DOOM_GENMIDI_LUMP_BYTES, &g), DOOM_GENMIDI_OK);
    CU_ASSERT_EQUAL(doom_genmidi_instr(&g, 0, NULL), DOOM_GENMIDI_EINVAL);
}

static void test_out_of_range_leaves_out_untouched(void)
{
    doom_genmidi_t g;
    doom_genmidi_instr_t in;

    syn_build();
    CU_ASSERT_EQUAL(doom_genmidi_parse(SYN, DOOM_GENMIDI_LUMP_BYTES, &g), DOOM_GENMIDI_OK);

    fill_sentinel(&in);
    CU_ASSERT_EQUAL(doom_genmidi_instr(&g, DOOM_GENMIDI_NUM_INSTRS, &in), DOOM_GENMIDI_EINVAL);
    CU_ASSERT_EQUAL(doom_genmidi_instr(&g, 0xFFFFFFFFu, &in), DOOM_GENMIDI_EINVAL);
    CU_ASSERT_EQUAL(doom_genmidi_program(&g, 128, &in), DOOM_GENMIDI_EINVAL);
    CU_ASSERT_EQUAL(doom_genmidi_percussion(&g, 34, &in), DOOM_GENMIDI_EINVAL);
    CU_ASSERT_EQUAL(doom_genmidi_percussion(&g, 82, &in), DOOM_GENMIDI_EINVAL);
    CU_ASSERT_EQUAL(doom_genmidi_percussion(&g, 0, &in), DOOM_GENMIDI_EINVAL);
    CU_ASSERT_TRUE(is_sentinel(&in));
}

static void test_program_and_percussion_map_to_records(void)
{
    doom_genmidi_t g;
    doom_genmidi_instr_t in;

    /* Tag every record with its own index in fixed_note. */
    syn_build();
    for (uint32_t i = 0; i < DOOM_GENMIDI_NUM_INSTRS; i++) {
        syn_record(i)[3] = (uint8_t)i;
    }
    CU_ASSERT_EQUAL(doom_genmidi_parse(SYN, DOOM_GENMIDI_LUMP_BYTES, &g), DOOM_GENMIDI_OK);

    CU_ASSERT_EQUAL(doom_genmidi_program(&g, 0, &in), DOOM_GENMIDI_OK);
    CU_ASSERT_EQUAL(in.fixed_note, 0);
    CU_ASSERT_EQUAL(doom_genmidi_program(&g, 127, &in), DOOM_GENMIDI_OK);
    CU_ASSERT_EQUAL(in.fixed_note, 127);
    CU_ASSERT_EQUAL(doom_genmidi_percussion(&g, 35, &in), DOOM_GENMIDI_OK);
    CU_ASSERT_EQUAL(in.fixed_note, 128);
    CU_ASSERT_EQUAL(doom_genmidi_percussion(&g, 69, &in), DOOM_GENMIDI_OK);
    CU_ASSERT_EQUAL(in.fixed_note, 162);
    CU_ASSERT_EQUAL(doom_genmidi_percussion(&g, 81, &in), DOOM_GENMIDI_OK);
    CU_ASSERT_EQUAL(in.fixed_note, 174);
}

static void test_names_are_always_terminated(void)
{
    doom_genmidi_t g;
    char name[33];

    syn_build();
    uint8_t *names = SYN + DOOM_GENMIDI_NAMES_OFFSET;
    memcpy(names + 3u * DOOM_GENMIDI_NAME_BYTES, "Short", 5);
    /* Record 4 fills all 32 bytes, and record 5's name follows with no NUL
     * between them: a reader that kept going would return 33+ characters. */
    memset(names + 4u * DOOM_GENMIDI_NAME_BYTES, 'X', DOOM_GENMIDI_NAME_BYTES);
    memcpy(names + 5u * DOOM_GENMIDI_NAME_BYTES, "Next", 4);
    /* The very last name, also full, ends where the lump ends. */
    memset(names + 174u * DOOM_GENMIDI_NAME_BYTES, 'Z', DOOM_GENMIDI_NAME_BYTES);
    CU_ASSERT_EQUAL(doom_genmidi_parse(SYN, DOOM_GENMIDI_LUMP_BYTES, &g), DOOM_GENMIDI_OK);

    doom_genmidi_name(&g, 3, name);
    CU_ASSERT_STRING_EQUAL(name, "Short");
    doom_genmidi_name(&g, 4, name);
    CU_ASSERT_EQUAL(strlen(name), 32u);
    CU_ASSERT_EQUAL(name[31], 'X');
    doom_genmidi_name(&g, 174, name);
    CU_ASSERT_EQUAL(strlen(name), 32u);
    doom_genmidi_name(&g, 0, name);
    CU_ASSERT_STRING_EQUAL(name, "");

    memset(name, 'Q', sizeof(name));
    doom_genmidi_name(&g, DOOM_GENMIDI_NUM_INSTRS, name);
    CU_ASSERT_STRING_EQUAL(name, "");
    memset(name, 'Q', sizeof(name));
    doom_genmidi_name(NULL, 0, name);
    CU_ASSERT_STRING_EQUAL(name, "");
}

/*
 * The PWAD-only case: a valid WAD with no GENMIDI in it.  Built by hand
 * (12-byte header, one 4-byte lump, one 16-byte directory entry) so the
 * absence is a property of the input rather than of whichever WAD is mounted.
 */
static void test_missing_genmidi_is_enoent(void)
{
    static uint8_t pwad[12 + 4 + 16];
    wad_t w;
    doom_genmidi_t g = { .lump = (const uint8_t *)0x1 };

    memset(pwad, 0, sizeof(pwad));
    memcpy(pwad, "PWAD", 4);
    pwad[4]  = 1;           /* numlumps */
    pwad[8]  = 16;          /* dirofs */
    memcpy(pwad + 12, "abcd", 4);
    pwad[16] = 12;          /* filepos */
    pwad[20] = 4;           /* size */
    memcpy(pwad + 24, "DEMO1", 5);

    CU_ASSERT_EQUAL(wad_init(&w, pwad, sizeof(pwad)), 0);
    CU_ASSERT_EQUAL(doom_genmidi_find(&w, &g), DOOM_GENMIDI_ENOENT);
    CU_ASSERT((g.lump) == ((const uint8_t *)0x1));
}

static void test_strerror_covers_every_code(void)
{
    const int codes[] = {
        DOOM_GENMIDI_OK, DOOM_GENMIDI_ENOENT, DOOM_GENMIDI_EINVAL, DOOM_GENMIDI_EFORMAT,
    };
    const char *unknown = doom_genmidi_strerror(12345);

    CU_ASSERT_PTR_NOT_NULL(unknown);
    for (uint32_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        const char *s = doom_genmidi_strerror(codes[i]);
        CU_ASSERT_PTR_NOT_NULL(s);
        CU_ASSERT(s != unknown);
        for (uint32_t j = 0; j < i; j++) {
            CU_ASSERT(s != doom_genmidi_strerror(codes[j]));
        }
    }
}

/* ---- the real freedoom2 GENMIDI ----------------------------------------- */

static const wad_t *real_wad(void)
{
    const doom_wad_t *w = doom_wad_mounted();
    return w != NULL ? &w->wad : NULL;
}

int suite_doom_genmidi_init(void)
{
    uint64_t paddr = 0, size = 0;

    if (doom_wad_disk_get(&paddr, &size) == 0) {
        (void)doom_wad_mount((const void *)(uintptr_t)paddr, (uint32_t)size);
    }
    return 0;
}

int suite_doom_genmidi_cleanup(void)
{
    doom_wad_unmount();
    return 0;
}

static int real_genmidi(doom_genmidi_t *g)
{
    const wad_t *w = real_wad();
    if (w == NULL) {
        return DOOM_GENMIDI_ENOENT;
    }
    return doom_genmidi_find(w, g);
}

static void test_real_genmidi_is_found(void)
{
    doom_genmidi_t g;

    CU_ASSERT_PTR_NOT_NULL(real_wad());
    CU_ASSERT_EQUAL(real_genmidi(&g), DOOM_GENMIDI_OK);
}

/*
 * Every record decodes and has a sounding voice 0 -- except #162, freedoom's
 * "Null Instrument" (percussion note 69), whose voices are both the silent
 * filler (output level 0x3f, no envelope).  Every two-voice record's second
 * voice has a real envelope too.  The flags histogram is asserted exactly:
 * it is the property the synth's voice allocation depends on.
 */
static void test_real_all_instruments_decode(void)
{
    doom_genmidi_t g;
    doom_genmidi_instr_t in;
    char name[33];
    uint32_t f0 = 0, f1 = 0, f4 = 0, f5 = 0, other = 0;
    uint32_t silent = 0, silent_index = 0, unnamed = 0, flat2 = 0, perc_fixed = 0;

    if (real_genmidi(&g) != DOOM_GENMIDI_OK) {
        CU_ASSERT(!"no real GENMIDI");
        return;
    }

    for (uint32_t i = 0; i < DOOM_GENMIDI_NUM_INSTRS; i++) {
        if (doom_genmidi_instr(&g, i, &in) != DOOM_GENMIDI_OK) {
            CU_ASSERT(!"real record failed to decode");
            return;
        }
        switch (in.flags) {
        case 0x0: f0++; break;
        case 0x1: f1++; break;
        case 0x4: f4++; break;
        case 0x5: f5++; break;
        default:  other++; break;
        }
        if ((in.voice[0].mod.attack | in.voice[0].car.attack) == 0) {
            silent++;
            silent_index = i;
        }
        if ((in.flags & DOOM_GENMIDI_FLAG_2VOICE) && in.voice[1].car.attack == 0) {
            flat2++;
        }
        if (i >= DOOM_GENMIDI_NUM_PROGRAMS && (in.flags & DOOM_GENMIDI_FLAG_FIXED)) {
            perc_fixed++;
        }
        doom_genmidi_name(&g, i, name);
        if (name[0] == '\0') {
            unnamed++;
        }
    }

    CU_ASSERT_EQUAL(f4, 94u);
    CU_ASSERT_EQUAL(f0, 32u);
    CU_ASSERT_EQUAL(f1, 25u);
    CU_ASSERT_EQUAL(f5, 24u);
    CU_ASSERT_EQUAL(other, 0u);
    CU_ASSERT_EQUAL(silent, 1u);
    CU_ASSERT_EQUAL(silent_index, 162u);
    CU_ASSERT_EQUAL(flat2, 0u);
    CU_ASSERT_EQUAL(perc_fixed, 46u);
    CU_ASSERT_EQUAL(unnamed, 0u);
}

static void test_real_known_records(void)
{
    doom_genmidi_t g;
    doom_genmidi_instr_t in;
    char name[33];
    static const uint8_t silent_op[6] = { 0, 0, 0, 0, 0, 0x3F };

    if (real_genmidi(&g) != DOOM_GENMIDI_OK) {
        CU_ASSERT(!"no real GENMIDI");
        return;
    }

    /* #0 Acoustic Grand Piano: two-voice, both an octave down. */
    CU_ASSERT_EQUAL(doom_genmidi_instr(&g, 0, &in), DOOM_GENMIDI_OK);
    CU_ASSERT_EQUAL(in.flags, DOOM_GENMIDI_FLAG_2VOICE);
    CU_ASSERT_EQUAL(in.fine_tune, 130);
    CU_ASSERT_EQUAL(in.fixed_note, 0);
    {
        static const uint8_t m0[6] = { 0x33, 0xE1, 0x23, 0x02, 0x80, 0x25 };
        static const uint8_t c0[6] = { 0x31, 0xF1, 0xF4, 0x04, 0x00, 0x09 };
        static const uint8_t m1[6] = { 0x32, 0xF1, 0x23, 0x02, 0x00, 0x24 };
        static const uint8_t c1[6] = { 0x31, 0xF1, 0xF4, 0x00, 0x00, 0x0A };
        check_voice(&in.voice[0], m0, 0x0E, c0, -12);
        check_voice(&in.voice[1], m1, 0x0E, c1, -12);
    }
    doom_genmidi_name(&g, 0, name);
    CU_ASSERT_STRING_EQUAL(name, "Acoustic Grand Piano");

    /* #29 Overdriven Guitar: the two voices have different offsets. */
    CU_ASSERT_EQUAL(doom_genmidi_program(&g, 29, &in), DOOM_GENMIDI_OK);
    CU_ASSERT_EQUAL(in.flags, DOOM_GENMIDI_FLAG_2VOICE);
    CU_ASSERT_EQUAL(in.fine_tune, 6);
    {
        static const uint8_t m0[6] = { 0x11, 0x81, 0xF7, 0x00, 0x40, 0x00 };
        static const uint8_t c0[6] = { 0x54, 0xF0, 0xF7, 0x03, 0x40, 0x05 };
        static const uint8_t m1[6] = { 0x40, 0x81, 0xF7, 0x00, 0x40, 0x00 };
        static const uint8_t c1[6] = { 0x40, 0xF0, 0xF7, 0x03, 0x00, 0x01 };
        check_voice(&in.voice[0], m0, 0x08, c0, -12);
        check_voice(&in.voice[1], m1, 0x0C, c1, 2);
    }
    doom_genmidi_name(&g, 29, name);
    CU_ASSERT_STRING_EQUAL(name, "Overdriven Guitar");

    /* #128 = percussion note 35, Acoustic Bass Drum: fixed pitch, one voice. */
    CU_ASSERT_EQUAL(doom_genmidi_percussion(&g, 35, &in), DOOM_GENMIDI_OK);
    CU_ASSERT_EQUAL(in.flags, DOOM_GENMIDI_FLAG_FIXED);
    CU_ASSERT_EQUAL(in.fine_tune, 128);
    CU_ASSERT_EQUAL(in.fixed_note, 25);
    {
        static const uint8_t m0[6] = { 0x00, 0xFB, 0x57, 0x00, 0x00, 0x00 };
        static const uint8_t c0[6] = { 0x00, 0xF8, 0x46, 0x00, 0x00, 0x00 };
        check_voice(&in.voice[0], m0, 0x00, c0, 0);
        check_voice(&in.voice[1], silent_op, 0x00, silent_op, 0);
    }
    doom_genmidi_name(&g, 128, name);
    CU_ASSERT_STRING_EQUAL(name, "Acoustic Bass Drum");

    /* #138 = note 45, Low Tom. */
    CU_ASSERT_EQUAL(doom_genmidi_percussion(&g, 45, &in), DOOM_GENMIDI_OK);
    CU_ASSERT_EQUAL(in.flags, DOOM_GENMIDI_FLAG_FIXED);
    CU_ASSERT_EQUAL(in.fixed_note, 37);
    {
        static const uint8_t m0[6] = { 0x01, 0xF6, 0x08, 0x05, 0x00, 0x0C };
        static const uint8_t c0[6] = { 0x00, 0xF6, 0x04, 0x01, 0x00, 0x00 };
        check_voice(&in.voice[0], m0, 0x08, c0, -12);
    }
    doom_genmidi_name(&g, 138, name);
    CU_ASSERT_STRING_EQUAL(name, "Low Tom");

    /* #174 = note 81, Open Triangle: the last record, next to the names. */
    CU_ASSERT_EQUAL(doom_genmidi_percussion(&g, 81, &in), DOOM_GENMIDI_OK);
    CU_ASSERT_EQUAL(in.flags, DOOM_GENMIDI_FLAG_FIXED);
    CU_ASSERT_EQUAL(in.fixed_note, 90);
    {
        static const uint8_t m0[6] = { 0x86, 0xF2, 0x60, 0x01, 0x40, 0x0F };
        static const uint8_t c0[6] = { 0x94, 0xF2, 0xB7, 0x00, 0x80, 0x00 };
        check_voice(&in.voice[0], m0, 0x08, c0, 0);
        check_voice(&in.voice[1], silent_op, 0x00, silent_op, 0);
    }
    doom_genmidi_name(&g, 174, name);
    CU_ASSERT_STRING_EQUAL(name, "Open Triangle");

    doom_genmidi_name(&g, 162, name);
    CU_ASSERT_STRING_EQUAL(name, "Null Instrument");
}

static void test_real_lookups_agree_with_records(void)
{
    doom_genmidi_t g;
    doom_genmidi_instr_t a, b;

    if (real_genmidi(&g) != DOOM_GENMIDI_OK) {
        CU_ASSERT(!"no real GENMIDI");
        return;
    }
    CU_ASSERT_EQUAL(doom_genmidi_program(&g, 0, &a), DOOM_GENMIDI_OK);
    CU_ASSERT_EQUAL(doom_genmidi_instr(&g, 0, &b), DOOM_GENMIDI_OK);
    CU_ASSERT_TRUE(instr_eq(&a, &b));
    CU_ASSERT_EQUAL(doom_genmidi_percussion(&g, 35, &a), DOOM_GENMIDI_OK);
    CU_ASSERT_EQUAL(doom_genmidi_instr(&g, 128, &b), DOOM_GENMIDI_OK);
    CU_ASSERT_TRUE(instr_eq(&a, &b));
    CU_ASSERT_EQUAL(doom_genmidi_instr(&g, 1, &b), DOOM_GENMIDI_OK);
    CU_ASSERT_FALSE(instr_eq(&a, &b));
}

/* A real lump cut short by one byte is refused, never partially decoded. */
static void test_truncated_real_lump_is_einval(void)
{
    const wad_t *w = real_wad();
    uint32_t size = 0;
    doom_genmidi_t g = { .lump = (const uint8_t *)0x1 };

    if (w == NULL) {
        CU_ASSERT_PTR_NOT_NULL(w);
        return;
    }
    const uint8_t *lump = wad_find_lump(w, "GENMIDI", &size);
    CU_ASSERT_PTR_NOT_NULL(lump);
    CU_ASSERT_EQUAL(size, DOOM_GENMIDI_LUMP_BYTES);
    CU_ASSERT_EQUAL(doom_genmidi_parse(lump, size - 1u, &g), DOOM_GENMIDI_EINVAL);
    CU_ASSERT((g.lump) == ((const uint8_t *)0x1));
}

void suite_doom_genmidi_tests(CU_pSuite s)
{
    CU_add_test(s, "layout constants add up to 11908 bytes",
                test_layout_constants);
    CU_add_test(s, "a synthetic record decodes byte for byte",
                test_synthetic_record_decodes_exactly);
    CU_add_test(s, "a bad magic is EFORMAT",
                test_bad_magic_is_eformat);
    CU_add_test(s, "any size but 11908 is EINVAL, checked before the magic",
                test_wrong_size_is_einval);
    CU_add_test(s, "NULL arguments are EINVAL",
                test_null_args_are_einval);
    CU_add_test(s, "out-of-range lookups leave *out untouched",
                test_out_of_range_leaves_out_untouched);
    CU_add_test(s, "programs and percussion notes map to their records",
                test_program_and_percussion_map_to_records);
    CU_add_test(s, "names are NUL-terminated even at 32 characters",
                test_names_are_always_terminated);
    CU_add_test(s, "a WAD without GENMIDI is ENOENT",
                test_missing_genmidi_is_enoent);
    CU_add_test(s, "strerror names every result code",
                test_strerror_covers_every_code);

    CU_add_test(s, "the real freedoom2 GENMIDI is found",
                test_real_genmidi_is_found);
    CU_add_test(s, "all 175 real instruments decode",
                test_real_all_instruments_decode);
    CU_add_test(s, "known real records match field by field",
                test_real_known_records);
    CU_add_test(s, "program/percussion lookups agree with records",
                test_real_lookups_agree_with_records);
    CU_add_test(s, "a truncated real GENMIDI is EINVAL",
                test_truncated_real_lump_is_einval);
}
