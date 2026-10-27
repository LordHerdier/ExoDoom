#include "libos_doom/doom_opl.h"

#include "string.h"

/*
 * doom_opl.c — see doom_opl.h for the contract.
 *
 * The tables and the arithmetic of freq_for_voice() and set_voice_level() are
 * DMX's, as chocolate-doom's src/i_oplmusic.c (GPL-2.0+, Simon Howard)
 * reconstructs it; this file is a reimplementation over decoded GENMIDI
 * records and a single in-process chip rather than a port of that file's
 * structure.  Where this file departs from it, the comment says so.
 */

/* OPL register bases (low byte; | 0x100 selects the second array). */
#define REG_TREMOLO   0x20
#define REG_LEVEL     0x40
#define REG_ATTACK    0x60
#define REG_SUSTAIN   0x80
#define REG_FREQ_1    0xA0
#define REG_FREQ_2    0xB0     /* bit 5: key on */
#define REG_FEEDBACK  0xC0     /* bits 4/5: output A/B in OPL3 mode */
#define REG_WAVEFORM  0xE0
#define KEY_ON_BIT    0x20

/* Operator slot offsets of the nine channels of one array. */
static const uint8_t voice_ops[2][9] = {
    { 0x00, 0x01, 0x02, 0x08, 0x09, 0x0a, 0x10, 0x11, 0x12 },  /* modulator */
    { 0x03, 0x04, 0x05, 0x0b, 0x0c, 0x0d, 0x13, 0x14, 0x15 },  /* carrier */
};

/*
 * DMX's frequency table: register value (F-number, plus the block bits for
 * the low entries) at 32 steps per semitone.  The first 284 entries cover the
 * lowest notes outright; above that, entries 284.. are one octave's worth
 * reused with the block number added.  The final 0x36c is a buffer overrun
 * in the original, kept because DMX reads it.
 */
static const uint16_t frequency_curve[] = {
    0x133, 0x133, 0x134, 0x134, 0x135, 0x136, 0x136, 0x137,
    0x137, 0x138, 0x138, 0x139, 0x139, 0x13a, 0x13b, 0x13b,
    0x13c, 0x13c, 0x13d, 0x13d, 0x13e, 0x13f, 0x13f, 0x140,
    0x140, 0x141, 0x142, 0x142, 0x143, 0x143, 0x144, 0x144,
    0x145, 0x146, 0x146, 0x147, 0x147, 0x148, 0x149, 0x149,
    0x14a, 0x14a, 0x14b, 0x14c, 0x14c, 0x14d, 0x14d, 0x14e,
    0x14f, 0x14f, 0x150, 0x150, 0x151, 0x152, 0x152, 0x153,
    0x153, 0x154, 0x155, 0x155, 0x156, 0x157, 0x157, 0x158,
    0x158, 0x159, 0x15a, 0x15a, 0x15b, 0x15b, 0x15c, 0x15d,
    0x15d, 0x15e, 0x15f, 0x15f, 0x160, 0x161, 0x161, 0x162,
    0x162, 0x163, 0x164, 0x164, 0x165, 0x166, 0x166, 0x167,
    0x168, 0x168, 0x169, 0x16a, 0x16a, 0x16b, 0x16c, 0x16c,
    0x16d, 0x16e, 0x16e, 0x16f, 0x170, 0x170, 0x171, 0x172,
    0x172, 0x173, 0x174, 0x174, 0x175, 0x176, 0x176, 0x177,
    0x178, 0x178, 0x179, 0x17a, 0x17a, 0x17b, 0x17c, 0x17c,
    0x17d, 0x17e, 0x17e, 0x17f, 0x180, 0x181, 0x181, 0x182,
    0x183, 0x183, 0x184, 0x185, 0x185, 0x186, 0x187, 0x188,
    0x188, 0x189, 0x18a, 0x18a, 0x18b, 0x18c, 0x18d, 0x18d,
    0x18e, 0x18f, 0x18f, 0x190, 0x191, 0x192, 0x192, 0x193,
    0x194, 0x194, 0x195, 0x196, 0x197, 0x197, 0x198, 0x199,
    0x19a, 0x19a, 0x19b, 0x19c, 0x19d, 0x19d, 0x19e, 0x19f,
    0x1a0, 0x1a0, 0x1a1, 0x1a2, 0x1a3, 0x1a3, 0x1a4, 0x1a5,
    0x1a6, 0x1a6, 0x1a7, 0x1a8, 0x1a9, 0x1a9, 0x1aa, 0x1ab,
    0x1ac, 0x1ad, 0x1ad, 0x1ae, 0x1af, 0x1b0, 0x1b0, 0x1b1,
    0x1b2, 0x1b3, 0x1b4, 0x1b4, 0x1b5, 0x1b6, 0x1b7, 0x1b8,
    0x1b8, 0x1b9, 0x1ba, 0x1bb, 0x1bc, 0x1bc, 0x1bd, 0x1be,
    0x1bf, 0x1c0, 0x1c0, 0x1c1, 0x1c2, 0x1c3, 0x1c4, 0x1c4,
    0x1c5, 0x1c6, 0x1c7, 0x1c8, 0x1c9, 0x1c9, 0x1ca, 0x1cb,
    0x1cc, 0x1cd, 0x1ce, 0x1ce, 0x1cf, 0x1d0, 0x1d1, 0x1d2,
    0x1d3, 0x1d3, 0x1d4, 0x1d5, 0x1d6, 0x1d7, 0x1d8, 0x1d8,
    0x1d9, 0x1da, 0x1db, 0x1dc, 0x1dd, 0x1de, 0x1de, 0x1df,
    0x1e0, 0x1e1, 0x1e2, 0x1e3, 0x1e4, 0x1e5, 0x1e5, 0x1e6,
    0x1e7, 0x1e8, 0x1e9, 0x1ea, 0x1eb, 0x1ec, 0x1ed, 0x1ed,
    0x1ee, 0x1ef, 0x1f0, 0x1f1, 0x1f2, 0x1f3, 0x1f4, 0x1f5,
    0x1f6, 0x1f6, 0x1f7, 0x1f8, 0x1f9, 0x1fa, 0x1fb, 0x1fc,
    0x1fd, 0x1fe, 0x1ff, 0x200, 0x201, 0x201, 0x202, 0x203,
    /* 284: the octave reused for every block from here up */
    0x204, 0x205, 0x206, 0x207, 0x208, 0x209, 0x20a, 0x20b,
    0x20c, 0x20d, 0x20e, 0x20f, 0x210, 0x210, 0x211, 0x212,
    0x213, 0x214, 0x215, 0x216, 0x217, 0x218, 0x219, 0x21a,
    0x21b, 0x21c, 0x21d, 0x21e, 0x21f, 0x220, 0x221, 0x222,
    0x223, 0x224, 0x225, 0x226, 0x227, 0x228, 0x229, 0x22a,
    0x22b, 0x22c, 0x22d, 0x22e, 0x22f, 0x230, 0x231, 0x232,
    0x233, 0x234, 0x235, 0x236, 0x237, 0x238, 0x239, 0x23a,
    0x23b, 0x23c, 0x23d, 0x23e, 0x23f, 0x240, 0x241, 0x242,
    0x244, 0x245, 0x246, 0x247, 0x248, 0x249, 0x24a, 0x24b,
    0x24c, 0x24d, 0x24e, 0x24f, 0x250, 0x251, 0x252, 0x253,
    0x254, 0x256, 0x257, 0x258, 0x259, 0x25a, 0x25b, 0x25c,
    0x25d, 0x25e, 0x25f, 0x260, 0x262, 0x263, 0x264, 0x265,
    0x266, 0x267, 0x268, 0x269, 0x26a, 0x26c, 0x26d, 0x26e,
    0x26f, 0x270, 0x271, 0x272, 0x273, 0x275, 0x276, 0x277,
    0x278, 0x279, 0x27a, 0x27b, 0x27d, 0x27e, 0x27f, 0x280,
    0x281, 0x282, 0x284, 0x285, 0x286, 0x287, 0x288, 0x289,
    0x28b, 0x28c, 0x28d, 0x28e, 0x28f, 0x290, 0x292, 0x293,
    0x294, 0x295, 0x296, 0x298, 0x299, 0x29a, 0x29b, 0x29c,
    0x29e, 0x29f, 0x2a0, 0x2a1, 0x2a2, 0x2a4, 0x2a5, 0x2a6,
    0x2a7, 0x2a9, 0x2aa, 0x2ab, 0x2ac, 0x2ae, 0x2af, 0x2b0,
    0x2b1, 0x2b2, 0x2b4, 0x2b5, 0x2b6, 0x2b7, 0x2b9, 0x2ba,
    0x2bb, 0x2bd, 0x2be, 0x2bf, 0x2c0, 0x2c2, 0x2c3, 0x2c4,
    0x2c5, 0x2c7, 0x2c8, 0x2c9, 0x2cb, 0x2cc, 0x2cd, 0x2ce,
    0x2d0, 0x2d1, 0x2d2, 0x2d4, 0x2d5, 0x2d6, 0x2d8, 0x2d9,
    0x2da, 0x2dc, 0x2dd, 0x2de, 0x2e0, 0x2e1, 0x2e2, 0x2e4,
    0x2e5, 0x2e6, 0x2e8, 0x2e9, 0x2ea, 0x2ec, 0x2ed, 0x2ee,
    0x2f0, 0x2f1, 0x2f2, 0x2f4, 0x2f5, 0x2f6, 0x2f8, 0x2f9,
    0x2fb, 0x2fc, 0x2fd, 0x2ff, 0x300, 0x302, 0x303, 0x304,
    0x306, 0x307, 0x309, 0x30a, 0x30b, 0x30d, 0x30e, 0x310,
    0x311, 0x312, 0x314, 0x315, 0x317, 0x318, 0x31a, 0x31b,
    0x31c, 0x31e, 0x31f, 0x321, 0x322, 0x324, 0x325, 0x327,
    0x328, 0x329, 0x32b, 0x32c, 0x32e, 0x32f, 0x331, 0x332,
    0x334, 0x335, 0x337, 0x338, 0x33a, 0x33b, 0x33d, 0x33e,
    0x340, 0x341, 0x343, 0x344, 0x346, 0x347, 0x349, 0x34a,
    0x34c, 0x34d, 0x34f, 0x350, 0x352, 0x353, 0x355, 0x357,
    0x358, 0x35a, 0x35b, 0x35d, 0x35e, 0x360, 0x361, 0x363,
    0x365, 0x366, 0x368, 0x369, 0x36b, 0x36c, 0x36e, 0x370,
    0x371, 0x373, 0x374, 0x376, 0x378, 0x379, 0x37b, 0x37c,
    0x37e, 0x380, 0x381, 0x383, 0x384, 0x386, 0x388, 0x389,
    0x38b, 0x38d, 0x38e, 0x390, 0x392, 0x393, 0x395, 0x397,
    0x398, 0x39a, 0x39c, 0x39d, 0x39f, 0x3a1, 0x3a2, 0x3a4,
    0x3a6, 0x3a7, 0x3a9, 0x3ab, 0x3ac, 0x3ae, 0x3b0, 0x3b1,
    0x3b3, 0x3b5, 0x3b7, 0x3b8, 0x3ba, 0x3bc, 0x3bd, 0x3bf,
    0x3c1, 0x3c3, 0x3c4, 0x3c6, 0x3c8, 0x3ca, 0x3cb, 0x3cd,
    0x3cf, 0x3d1, 0x3d2, 0x3d4, 0x3d6, 0x3d8, 0x3da, 0x3db,
    0x3dd, 0x3df, 0x3e1, 0x3e3, 0x3e4, 0x3e6, 0x3e8, 0x3ea,
    0x3ec, 0x3ed, 0x3ef, 0x3f1, 0x3f3, 0x3f5, 0x3f6, 0x3f8,
    0x3fa, 0x3fc, 0x3fe, 0x36c,
};

#define FREQ_LOOP_START  284u
#define FREQ_LOOP_LEN    (12u * 32u)

/* DMX's MIDI volume (0..127) to OPL attenuation-complement curve. */
static const uint8_t volume_mapping_table[128] = {
    0, 1, 3, 5, 6, 8, 10, 11,
    13, 14, 16, 17, 19, 20, 22, 23,
    25, 26, 27, 29, 30, 32, 33, 34,
    36, 37, 39, 41, 43, 45, 47, 49,
    50, 52, 54, 55, 57, 59, 60, 61,
    63, 64, 66, 67, 68, 69, 71, 72,
    73, 74, 75, 76, 77, 79, 80, 81,
    82, 83, 84, 84, 85, 86, 87, 88,
    89, 90, 91, 92, 92, 93, 94, 95,
    96, 96, 97, 98, 99, 99, 100, 101,
    101, 102, 103, 103, 104, 105, 105, 106,
    107, 107, 108, 109, 109, 110, 110, 111,
    112, 112, 113, 113, 114, 114, 115, 115,
    116, 117, 117, 118, 118, 119, 119, 120,
    120, 121, 121, 122, 122, 123, 123, 123,
    124, 124, 125, 125, 126, 126, 127, 127,
};

/* ---- registers ---------------------------------------------------------- */

static uint16_t array_bit(uint32_t v)  { return (uint16_t)((v / 9u) << 8); }
static uint8_t  chan_idx(uint32_t v)   { return (uint8_t)(v % 9u); }
static uint16_t op_reg(uint32_t v, uint8_t base, int carrier)
{
    return (uint16_t)(array_bit(v) | (uint16_t)(base + voice_ops[carrier][chan_idx(v)]));
}
static uint16_t ch_reg(uint32_t v, uint8_t base)
{
    return (uint16_t)(array_bit(v) | (uint16_t)(base + chan_idx(v)));
}

static void wr(doom_opl_t *s, uint16_t reg, uint8_t val)
{
    OPL3_WriteReg(&s->chip, reg, val);
}

static const doom_genmidi_voice_t *gm_voice(const doom_opl_voice_t *v)
{
    return &v->instr.voice[v->second];
}

/* An additive (connection = 1) voice hears its modulator directly, so its
 * level has to follow the note's volume too. */
static int additive(const doom_genmidi_voice_t *gv)
{
    return (gv->feedback & 0x01u) != 0;
}

static void load_operator(doom_opl_t *s, uint16_t level_reg,
                          const doom_genmidi_op_t *op, int max_level,
                          uint8_t *shadow)
{
    uint8_t level = (uint8_t)(op->scale | (max_level ? 0x3fu : op->level));
    uint16_t off = (uint16_t)(level_reg - REG_LEVEL);

    *shadow = level;
    wr(s, level_reg, level);
    wr(s, (uint16_t)(off + REG_TREMOLO),  op->tremolo);
    wr(s, (uint16_t)(off + REG_ATTACK),   op->attack);
    wr(s, (uint16_t)(off + REG_SUSTAIN),  op->sustain);
    wr(s, (uint16_t)(off + REG_WAVEFORM), op->waveform);
}

/* Carrier (and an additive modulator) start at full attenuation; the level
 * arrives in set_voice_level(), before key-on, as in DMX. */
static void program_voice(doom_opl_t *s, uint32_t v)
{
    doom_opl_voice_t *vo = &s->voices[v];
    const doom_genmidi_voice_t *gv = gm_voice(vo);

    load_operator(s, op_reg(v, REG_LEVEL, 1), &gv->car, 1, &vo->car_level);
    load_operator(s, op_reg(v, REG_LEVEL, 0), &gv->mod, additive(gv), &vo->mod_level);
    wr(s, ch_reg(v, REG_FEEDBACK),
       (uint8_t)((gv->feedback & 0x0fu) | s->chans[vo->midi_chan].pan));
}

static void set_voice_level(doom_opl_t *s, uint32_t v)
{
    doom_opl_voice_t *vo = &s->voices[v];
    const doom_opl_chan_t *ch = &s->chans[vo->midi_chan];
    const doom_genmidi_voice_t *gv = gm_voice(vo);

    /* Channel volume, expression and the master volume multiply; DMX instead
     * clamps the channel volume to the master, which leaves expression
     * nowhere to go.  The product then goes through DMX's own arithmetic. */
    uint32_t chan_vol = (uint32_t)ch->volume * ch->expression / 127u;
    chan_vol = chan_vol * s->master / 127u;

    uint32_t midi_volume = 2u * (volume_mapping_table[chan_vol] + 1u);
    uint32_t full = (volume_mapping_table[vo->velocity] * midi_volume) >> 9;
    uint8_t  car  = (uint8_t)(0x3fu - full);

    vo->car_level = (uint8_t)((vo->car_level & 0xc0u) | car);
    wr(s, op_reg(v, REG_LEVEL, 1), vo->car_level);

    if (additive(gv) && gv->mod.level != 0x3fu) {
        uint8_t mod = gv->mod.level;
        if (mod < car) {
            mod = car;
        }
        vo->mod_level = (uint8_t)((gv->mod.scale & 0xc0u) | mod);
        wr(s, op_reg(v, REG_LEVEL, 0), vo->mod_level);
    }
}

static uint16_t freq_for_voice(const doom_opl_t *s, const doom_opl_voice_t *vo)
{
    int32_t note = vo->note;

    if ((vo->instr.flags & DOOM_GENMIDI_FLAG_FIXED) == 0) {
        note += gm_voice(vo)->base_note_offset;
    }
    while (note < 0)  { note += 12; }
    while (note > 95) { note -= 12; }

    int32_t idx = 64 + 32 * note + s->chans[vo->midi_chan].bend;
    if (vo->second) {
        idx += (int32_t)(vo->instr.fine_tune / 2u) - 64;
    }
    if (idx < 0) {
        idx = 0;
    }
    if ((uint32_t)idx < FREQ_LOOP_START) {
        return frequency_curve[idx];
    }

    uint32_t sub    = ((uint32_t)idx - FREQ_LOOP_START) % FREQ_LOOP_LEN;
    uint32_t octave = ((uint32_t)idx - FREQ_LOOP_START) / FREQ_LOOP_LEN;
    if (octave > 7u) {
        octave = 7u;
    }
    return (uint16_t)(frequency_curve[sub + FREQ_LOOP_START] | (octave << 10));
}

static void write_freq(doom_opl_t *s, uint32_t v, int key_on)
{
    uint16_t f = s->voices[v].freq;

    wr(s, ch_reg(v, REG_FREQ_1), (uint8_t)(f & 0xffu));
    wr(s, ch_reg(v, REG_FREQ_2), (uint8_t)((f >> 8) | (key_on ? KEY_ON_BIT : 0u)));
}

/* ---- voice allocation --------------------------------------------------- */

static void release_voice(doom_opl_t *s, uint32_t v)
{
    if (!s->voices[v].in_use) {
        return;
    }
    write_freq(s, v, 0);            /* key off: the release envelope runs on */
    s->voices[v].in_use = 0;
}

/* The oldest in-use voice, preferring a two-voice note's second half. */
static uint32_t steal_candidate(const doom_opl_t *s)
{
    uint32_t best = DOOM_OPL_VOICES;

    for (uint32_t pass = 0; pass < 2 && best == DOOM_OPL_VOICES; pass++) {
        for (uint32_t v = 0; v < DOOM_OPL_VOICES; v++) {
            const doom_opl_voice_t *vo = &s->voices[v];
            if (!vo->in_use || (pass == 0 && !vo->second)) {
                continue;
            }
            if (best == DOOM_OPL_VOICES || vo->age < s->voices[best].age) {
                best = v;
            }
        }
    }
    return best;
}

static uint32_t free_voice(const doom_opl_t *s)
{
    for (uint32_t v = 0; v < DOOM_OPL_VOICES; v++) {
        if (!s->voices[v].in_use) {
            return v;
        }
    }
    return DOOM_OPL_VOICES;
}

static void voice_key_on(doom_opl_t *s, uint32_t v, uint8_t midi_chan,
                         const doom_genmidi_instr_t *instr, uint8_t second,
                         uint8_t key, uint8_t note, uint8_t velocity)
{
    doom_opl_voice_t *vo = &s->voices[v];

    vo->in_use    = 1;
    vo->midi_chan = midi_chan;
    vo->key       = key;
    vo->note      = (instr->flags & DOOM_GENMIDI_FLAG_FIXED) ? instr->fixed_note : note;
    vo->velocity  = velocity;
    vo->second    = second;
    vo->age       = s->next_age++;
    vo->instr     = *instr;

    program_voice(s, v);
    set_voice_level(s, v);
    vo->freq = freq_for_voice(s, vo);
    write_freq(s, v, 1);
}

static void note_off(doom_opl_t *s, uint8_t chan, uint8_t key)
{
    for (uint32_t v = 0; v < DOOM_OPL_VOICES; v++) {
        const doom_opl_voice_t *vo = &s->voices[v];
        if (vo->in_use && vo->midi_chan == chan && vo->key == key) {
            release_voice(s, v);
        }
    }
}

static void note_on(doom_opl_t *s, uint8_t chan, uint8_t key, uint8_t velocity)
{
    doom_genmidi_instr_t instr;
    uint8_t note = key;

    if (velocity == 0) {
        note_off(s, chan, key);
        return;
    }
    if (chan == DOOM_OPL_PERC_CHAN) {
        if (doom_genmidi_percussion(&s->genmidi, key, &instr) != DOOM_GENMIDI_OK) {
            return;                         /* outside GM 35..81 */
        }
        note = 60;                          /* DMX's; FLAG_FIXED overrides it */
    } else if (doom_genmidi_program(&s->genmidi, s->chans[chan].program,
                                    &instr) != DOOM_GENMIDI_OK) {
        return;
    }

    /* A key already sounding on this channel is retriggered, not doubled. */
    note_off(s, chan, key);

    uint32_t v = free_voice(s);
    if (v == DOOM_OPL_VOICES) {
        v = steal_candidate(s);
        release_voice(s, v);
    }
    voice_key_on(s, v, chan, &instr, 0, key, note, velocity);

    if (instr.flags & DOOM_GENMIDI_FLAG_2VOICE) {
        v = free_voice(s);                  /* the second half never steals */
        if (v != DOOM_OPL_VOICES) {
            voice_key_on(s, v, chan, &instr, 1, key, note, velocity);
        }
    }
}

static void update_chan_levels(doom_opl_t *s, uint8_t chan)
{
    for (uint32_t v = 0; v < DOOM_OPL_VOICES; v++) {
        if (s->voices[v].in_use && s->voices[v].midi_chan == chan) {
            set_voice_level(s, v);
        }
    }
}

static void update_chan_freqs(doom_opl_t *s, uint8_t chan)
{
    for (uint32_t v = 0; v < DOOM_OPL_VOICES; v++) {
        doom_opl_voice_t *vo = &s->voices[v];
        if (vo->in_use && vo->midi_chan == chan) {
            vo->freq = freq_for_voice(s, vo);
            write_freq(s, v, 1);
        }
    }
}

static void chan_notes_off(doom_opl_t *s, uint8_t chan)
{
    for (uint32_t v = 0; v < DOOM_OPL_VOICES; v++) {
        if (s->voices[v].in_use && s->voices[v].midi_chan == chan) {
            release_voice(s, v);
        }
    }
}

/*
 * Pan, after DMX: its OPL3 support had the stereo channels reversed, and
 * chocolate-doom keeps that by default, so a pan of 96+ ("right") lands on
 * output A, which Nuked mixes left.  This keeps it too -- the music is meant
 * to sound the way Doom's did.
 */
static uint8_t pan_bits(uint8_t pan)
{
    if (pan >= 96) {
        return 0x10;
    }
    if (pan <= 48) {
        return 0x20;
    }
    return 0x30;
}

static void controller(doom_opl_t *s, uint8_t chan, uint8_t cc, uint8_t val)
{
    doom_opl_chan_t *ch = &s->chans[chan];

    switch (cc) {
    case 7:                                 /* channel volume */
        ch->volume = val;
        update_chan_levels(s, chan);
        break;
    case 11:                                /* expression */
        ch->expression = val;
        update_chan_levels(s, chan);
        break;
    case 10: {                              /* pan */
        uint8_t bits = pan_bits(val);
        if (bits != ch->pan) {
            ch->pan = bits;
            for (uint32_t v = 0; v < DOOM_OPL_VOICES; v++) {
                const doom_opl_voice_t *vo = &s->voices[v];
                if (vo->in_use && vo->midi_chan == chan) {
                    wr(s, ch_reg(v, REG_FEEDBACK),
                       (uint8_t)((gm_voice(vo)->feedback & 0x0fu) | bits));
                }
            }
        }
        break;
    }
    case 121:                               /* reset all controllers */
        ch->expression = 127;
        ch->bend = 0;
        update_chan_levels(s, chan);
        update_chan_freqs(s, chan);
        break;
    case 120:                               /* all sound off */
    case 123:                               /* all notes off */
        chan_notes_off(s, chan);
        break;
    default:
        break;
    }
}

/* ---- public ------------------------------------------------------------- */

int doom_opl_init(doom_opl_t *s, const doom_genmidi_t *genmidi)
{
    if (s == NULL || genmidi == NULL || genmidi->lump == NULL) {
        return DOOM_OPL_EINVAL;
    }

    memset(s, 0, sizeof(*s));
    s->genmidi = *genmidi;
    s->master  = 127;
    for (uint32_t c = 0; c < DOOM_OPL_MIDI_CHANS; c++) {
        s->chans[c].volume     = 100;
        s->chans[c].expression = 127;
        s->chans[c].pan        = 0x30;
    }

    OPL3_Reset(&s->chip, DOOM_OPL_NATIVE_HZ);

    /* OPL3 mode first, so both arrays' channels are stereo-addressable;
     * no 4-operator pairs.  Then DMX's quiet register image, per array. */
    wr(s, 0x105, 0x01);
    wr(s, 0x104, 0x00);
    for (uint16_t a = 0; a <= 0x100; a = (uint16_t)(a + 0x100)) {
        for (uint16_t r = 0x40; r <= 0x55; r++) {
            wr(s, (uint16_t)(a | r), 0x3f);
        }
        for (uint16_t r = 0x60; r <= 0xf5; r++) {
            wr(s, (uint16_t)(a | r), 0x00);
        }
        for (uint16_t r = 0x01; r < 0x40; r++) {
            /* 0x01 bit 5: "waveform select enable", meaningful in array 0 */
            wr(s, (uint16_t)(a | r), (a == 0 && r == 0x01) ? 0x20 : 0x00);
        }
    }
    wr(s, 0x105, 0x01);                     /* that loop cleared 0x105 again */
    return DOOM_OPL_OK;
}

void doom_opl_event(doom_opl_t *s, const doom_midi_event_t *ev)
{
    if (s == NULL || ev == NULL) {
        return;
    }

    uint8_t chan = (uint8_t)(ev->status & 0x0fu);

    switch (ev->status & 0xf0u) {
    case 0x80:
        note_off(s, chan, ev->data1);
        break;
    case 0x90:
        note_on(s, chan, ev->data1, ev->data2);
        break;
    case 0xb0:
        controller(s, chan, ev->data1, ev->data2);
        break;
    case 0xc0:
        s->chans[chan].program = (uint8_t)(ev->data1 & 0x7fu);
        break;
    case 0xe0:                              /* MSB only, as Doom */
        s->chans[chan].bend = (int8_t)((int)(ev->data2 & 0x7fu) - 64);
        update_chan_freqs(s, chan);
        break;
    default:                                /* aftertouch, channel pressure */
        break;
    }
}

void doom_opl_set_volume(doom_opl_t *s, uint8_t volume)
{
    if (s == NULL) {
        return;
    }
    s->master = volume > 127u ? 127u : volume;
    for (uint32_t v = 0; v < DOOM_OPL_VOICES; v++) {
        if (s->voices[v].in_use) {
            set_voice_level(s, v);
        }
    }
}

void doom_opl_all_notes_off(doom_opl_t *s)
{
    if (s == NULL) {
        return;
    }
    for (uint32_t v = 0; v < DOOM_OPL_VOICES; v++) {
        release_voice(s, v);
    }
}

/*
 * The resampler.  NATIVE/OUTPUT is 49716/48000 native samples per output
 * frame; `acc` carries the remainder in 1/OUTPUT_HZ native samples, so the
 * step is exact and 48000 frames consume exactly 49716 native samples --
 * where a 16.16 truncated step (67875) would run 0.006% flat and drift three
 * samples a second.  The interpolation weight is the 16-bit fraction of
 * acc/OUTPUT, by multiply: 2^32 / 48000 rounded down is 89478.
 */
#define WEIGHT_MUL  89478u

/*
 * True when no operator can make a sound: none keyed, every envelope at full
 * attenuation.  The chip is not silent then, quite: its sine is one's
 * complement, so a fully attenuated operator still reads -1 on the negative
 * half of its phase, and a released channel's phase keeps running at the
 * note's frequency -- a 1-2 LSB square wave Nuked reproduces faithfully.
 * Freezing the phase instead would park it at 0 or -1 at random, which is
 * the DC offset the ticket forbids.  So the renderer writes true zeros
 * whenever this holds; with anything audible playing, the residue is far
 * below the music and left alone.
 */
static int chip_silent(const opl3_chip *chip)
{
    for (uint32_t i = 0; i < 36u; i++) {
        if (chip->slot[i].key || chip->slot[i].eg_rout < 0x1ffu) {
            return 0;
        }
    }
    return 1;
}

void doom_opl_render(doom_opl_t *s, int16_t *frames, uint32_t nframes)
{
    if (s == NULL || frames == NULL) {
        return;
    }

    for (uint32_t i = 0; i < nframes; i++) {
        s->acc += DOOM_OPL_NATIVE_HZ;
        while (s->acc >= DOOM_OPL_OUTPUT_HZ) {
            s->acc -= DOOM_OPL_OUTPUT_HZ;
            s->prev[0] = s->cur[0];
            s->prev[1] = s->cur[1];
            OPL3_Generate(&s->chip, s->cur);
            if (chip_silent(&s->chip)) {
                s->cur[0] = s->cur[1] = 0;
            }
            s->native_samples++;
        }

        if (s->master == 0) {
            frames[2 * i]     = 0;
            frames[2 * i + 1] = 0;
            continue;
        }

        int32_t w = (int32_t)((s->acc * WEIGHT_MUL) >> 16);   /* 0..65535 */
        for (int c = 0; c < 2; c++) {
            int32_t a = s->prev[c], b = s->cur[c];
            frames[2 * i + c] = (int16_t)(a + (int32_t)(((int64_t)(b - a) * w) >> 16));
        }
    }
}

uint32_t doom_opl_voices_in_use(const doom_opl_t *s)
{
    uint32_t n = 0;

    if (s == NULL) {
        return 0;
    }
    for (uint32_t v = 0; v < DOOM_OPL_VOICES; v++) {
        n += s->voices[v].in_use ? 1u : 0u;
    }
    return n;
}

uint32_t doom_opl_channels_keyed(const doom_opl_t *s)
{
    uint32_t n = 0;

    if (s == NULL) {
        return 0;
    }
    for (uint32_t c = 0; c < 18u; c++) {
        const opl3_channel *ch = &s->chip.channel[c];
        if (ch->slotz[0]->key || ch->slotz[1]->key) {
            n++;
        }
    }
    return n;
}
