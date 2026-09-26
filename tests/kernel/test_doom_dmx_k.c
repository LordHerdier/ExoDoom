/*
 * test_doom_dmx_k.c — SCRUM-211's acceptance tests for the DMX (`DS*`) sound
 * lump decoder.
 *
 * Two halves, for two different reasons.
 *
 * The synthetic half builds DMX lumps byte by byte here, because the inputs
 * that must be *rejected* do not exist as files to point at: a header whose
 * sample count runs past the end of the lump, a format tag that is not 3, a
 * lump that is nothing but padding.  This is the same argument
 * test_dg_init_k.c makes for its hand-built WADs.
 *
 * The real half decodes out of the actual freedoom2.wad, which is what the
 * ticket asks for ("against the vendored freedoom2.wad ... the way
 * test_fixed_math_k.c checks fixed-point trig against reference data").  That
 * is possible because `make docker-test` boots the ISO through GRUB, module
 * and all -- tests/kernel/test_vmm_fb_wad_k.c already depends on exactly
 * that, reading the WAD's magic bytes back through its identity mapping.
 * (test_dg_init_k.c's header comment claims a TESTING build never gets a GRUB
 * module.  That was true of a directly-booted kernel, not of the ISO CI
 * actually runs.)
 *
 * Every reference value below was extracted from build/freedoom2.wad, which
 * docker/scripts/build.sh pins by SHA-1.  They are exact on purpose: if the
 * pin moves, these should fail loudly rather than adapt, for the same reason
 * test_fixed_math_k.c compares all 24,577 table entries instead of spot
 * checking the shape of the curve.
 */

#include "kunit.h"

#include "doom_dmx.h"
#include "doom_wad.h"
#include "mmap.h"
#include "string.h"
#include "wad.h"

#include <stddef.h>
#include <stdint.h>

/* ---- freedoom2 v0.13.0 reference data ---------------------------------- */

/* DSPISTOL, the lump the ticket names.  Note the rate: 22050, not the 11025
 * the ticket's description assumes -- see src/doom_dmx.h on why that
 * assumption is wrong and why it matters. */
#define PISTOL_LUMP_SIZE   11034u
#define PISTOL_RATE_HZ     22050u
#define PISTOL_COUNT       11026u   /* the header's own sample count */
#define PISTOL_SAMPLES     10994u   /* == PISTOL_COUNT - 32 */

/* The first 8 samples *after* the 16-sample lead padding.  The raw lump body
 * starts 145, 144, 141, ... so this doubles as proof that the skip happened
 * rather than a separate assertion about it. */
static const uint8_t pistol_first8[8] = { 156, 162, 171, 172, 181, 201, 230, 255 };
/* The last 8 before the 16-sample trailing padding. */
static const uint8_t pistol_last8[8]  = { 127, 127, 127, 127, 127, 127, 127, 127 };
/* A window from the middle, so a decoder that got the start right by luck
 * and the stride wrong still fails. */
#define PISTOL_MID_OFFSET  5000u
static const uint8_t pistol_mid8[8]   = { 117, 122, 125, 136, 150, 156, 156, 157 };

/* A second lump at a different rate, so "reports the rate" is tested against
 * more than one answer. */
#define SHOTGN_RATE_HZ     11025u
#define SHOTGN_COUNT       11191u
#define SHOTGN_SAMPLES     11159u
static const uint8_t shotgn_first4[4] = { 140, 148, 152, 154 };

/* The whole DS namespace, as a histogram of sample rates.  This is the
 * assertion that stops the mixer from hardcoding one rate: two thirds of
 * these lumps are 22050 Hz, and the HDA stream is pinned at 48 kHz
 * (HDA_SAMPLE_RATE_HZ, src/hda.h), so resampling is unavoidable downstream. */
#define DS_LUMP_COUNT      109u
#define DS_COUNT_11025     38u
#define DS_COUNT_16000     1u
#define DS_COUNT_17990     2u
#define DS_COUNT_22050     67u
#define DS_COUNT_44100     1u

/* ---- synthetic DMX lumps ----------------------------------------------- */

#define FAKE_LUMP_LEN 256u

static uint8_t fake_lump[FAKE_LUMP_LEN];

/*
 * Build a DMX lump in fake_lump: the 8-byte header written raw (so a test can
 * supply values the parser must reject), then a body where sample i == i so
 * the padding skip is visible in the decoded values.
 */
static void make_dmx(uint16_t format, uint16_t rate, uint32_t count)
{
    memset(fake_lump, 0, sizeof(fake_lump));

    fake_lump[0] = (uint8_t)(format & 0xFFu);
    fake_lump[1] = (uint8_t)((format >> 8) & 0xFFu);
    fake_lump[2] = (uint8_t)(rate & 0xFFu);
    fake_lump[3] = (uint8_t)((rate >> 8) & 0xFFu);
    fake_lump[4] = (uint8_t)(count & 0xFFu);
    fake_lump[5] = (uint8_t)((count >> 8) & 0xFFu);
    fake_lump[6] = (uint8_t)((count >> 16) & 0xFFu);
    fake_lump[7] = (uint8_t)((count >> 24) & 0xFFu);

    for (uint32_t i = DOOM_DMX_HEADER_BYTES; i < FAKE_LUMP_LEN; i++) {
        fake_lump[i] = (uint8_t)(i - DOOM_DMX_HEADER_BYTES);
    }
}

/* ---- parse: rejections ------------------------------------------------- */

static void test_parse_rejects_null_and_short(void)
{
    doom_dmx_t dmx;

    CU_ASSERT_EQUAL(doom_dmx_parse(NULL, 64, &dmx), DOOM_DMX_EINVAL);

    make_dmx(DOOM_DMX_FORMAT_PCM, 11025, 64);
    CU_ASSERT_EQUAL(doom_dmx_parse(fake_lump, 0, &dmx), DOOM_DMX_EINVAL);
    /* One byte short of a header: the count field is not fully present, so
     * there is nothing to bounds-check against. */
    CU_ASSERT_EQUAL(doom_dmx_parse(fake_lump, DOOM_DMX_HEADER_BYTES - 1, &dmx),
                    DOOM_DMX_EINVAL);
    /* A bare header with a zero count is a format failure, not a length one:
     * the header itself is complete. */
    make_dmx(DOOM_DMX_FORMAT_PCM, 11025, 0);
    CU_ASSERT_EQUAL(doom_dmx_parse(fake_lump, DOOM_DMX_HEADER_BYTES, &dmx),
                    DOOM_DMX_ETOOSHORT);
}

static void test_parse_rejects_wrong_format_tag(void)
{
    doom_dmx_t dmx;

    for (uint16_t tag = 0; tag < 3; tag++) {
        make_dmx(tag, 11025, 64);
        CU_ASSERT_EQUAL(doom_dmx_parse(fake_lump, FAKE_LUMP_LEN, &dmx),
                        DOOM_DMX_EFORMAT);
    }

    /* Tag 3 with everything else identical must pass, or the loop above
     * proves nothing about the tag specifically. */
    make_dmx(DOOM_DMX_FORMAT_PCM, 11025, 64);
    CU_ASSERT_EQUAL(doom_dmx_parse(fake_lump, FAKE_LUMP_LEN, &dmx), DOOM_DMX_OK);
}

static void test_parse_rejects_count_past_lump(void)
{
    doom_dmx_t dmx;
    const uint32_t body = FAKE_LUMP_LEN - DOOM_DMX_HEADER_BYTES;

    /* Exactly the body length: legal, and what all 109 real lumps do. */
    make_dmx(DOOM_DMX_FORMAT_PCM, 11025, body);
    CU_ASSERT_EQUAL(doom_dmx_parse(fake_lump, FAKE_LUMP_LEN, &dmx), DOOM_DMX_OK);
    CU_ASSERT_EQUAL(dmx.num_samples, body - 2u * DOOM_DMX_PAD_SAMPLES);

    /* One sample past it: the malformation that would otherwise read off the
     * end of the mapping. */
    make_dmx(DOOM_DMX_FORMAT_PCM, 11025, body + 1u);
    CU_ASSERT_EQUAL(doom_dmx_parse(fake_lump, FAKE_LUMP_LEN, &dmx),
                    DOOM_DMX_EINVAL);

    /* Wildly past it, including a value that would wrap a count + 8 check. */
    make_dmx(DOOM_DMX_FORMAT_PCM, 11025, 0xFFFFFFFFu);
    CU_ASSERT_EQUAL(doom_dmx_parse(fake_lump, FAKE_LUMP_LEN, &dmx),
                    DOOM_DMX_EINVAL);
}

static void test_parse_rejects_padding_only_lump(void)
{
    doom_dmx_t dmx;

    /* Exactly 2 * 16 samples is all padding and no audio. */
    make_dmx(DOOM_DMX_FORMAT_PCM, 11025, 2u * DOOM_DMX_PAD_SAMPLES);
    CU_ASSERT_EQUAL(doom_dmx_parse(fake_lump, FAKE_LUMP_LEN, &dmx),
                    DOOM_DMX_ETOOSHORT);

    /* One more sample, and there is something to play. */
    make_dmx(DOOM_DMX_FORMAT_PCM, 11025, 2u * DOOM_DMX_PAD_SAMPLES + 1u);
    CU_ASSERT_EQUAL(doom_dmx_parse(fake_lump, FAKE_LUMP_LEN, &dmx), DOOM_DMX_OK);
    CU_ASSERT_EQUAL(dmx.num_samples, 1u);
}

static void test_parse_leaves_out_untouched_on_failure(void)
{
    doom_dmx_t dmx;

    dmx.format      = 0xBEEF;
    dmx.rate_hz     = 0xDEADBEEFu;
    dmx.samples     = NULL;
    dmx.num_samples = 0x5A5A5A5Au;

    make_dmx(1, 11025, 64);
    CU_ASSERT_EQUAL(doom_dmx_parse(fake_lump, FAKE_LUMP_LEN, &dmx),
                    DOOM_DMX_EFORMAT);

    /* A caller reusing one doom_dmx_t across a loop of lumps must not be able
     * to mistake a rejected lump's leftovers for a decoded one. */
    CU_ASSERT_EQUAL(dmx.format, 0xBEEF);
    CU_ASSERT_EQUAL(dmx.rate_hz, 0xDEADBEEFu);
    CU_ASSERT_PTR_NULL(dmx.samples);
    CU_ASSERT_EQUAL(dmx.num_samples, 0x5A5A5A5Au);
}

/* ---- parse: the happy path --------------------------------------------- */

static void test_parse_strips_padding_at_both_ends(void)
{
    doom_dmx_t     dmx;
    const uint32_t count = 200u;

    make_dmx(DOOM_DMX_FORMAT_PCM, 11025, count);
    CU_ASSERT_EQUAL(doom_dmx_parse(fake_lump, FAKE_LUMP_LEN, &dmx), DOOM_DMX_OK);

    CU_ASSERT_EQUAL(dmx.format, DOOM_DMX_FORMAT_PCM);
    CU_ASSERT_EQUAL(dmx.rate_hz, 11025u);
    CU_ASSERT_EQUAL(dmx.num_samples, count - 2u * DOOM_DMX_PAD_SAMPLES);

    /* The body is a ramp where sample i == i, so the lead padding having been
     * dropped is visible directly in the first value. */
    CU_ASSERT_EQUAL(dmx.samples[0], DOOM_DMX_PAD_SAMPLES);
    CU_ASSERT_EQUAL(dmx.samples[1], DOOM_DMX_PAD_SAMPLES + 1u);
    /* ...and the trailing padding in the last one. */
    CU_ASSERT_EQUAL(dmx.samples[dmx.num_samples - 1],
                    (uint8_t)(count - DOOM_DMX_PAD_SAMPLES - 1u));
}

static void test_parse_reports_rate_verbatim(void)
{
    doom_dmx_t dmx;
    /* Every rate that actually occurs in freedoom2, plus one nobody ships:
     * the decoder is not in the business of judging them. */
    static const uint16_t rates[] = { 11025, 16000, 17990, 22050, 44100, 8000 };

    for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        make_dmx(DOOM_DMX_FORMAT_PCM, rates[i], 64);
        CU_ASSERT_EQUAL(doom_dmx_parse(fake_lump, FAKE_LUMP_LEN, &dmx),
                        DOOM_DMX_OK);
        CU_ASSERT_EQUAL(dmx.rate_hz, rates[i]);
    }
}

/* ---- lump name composition -------------------------------------------- */

static void test_lump_name_composition(void)
{
    char name[9];

    /* The case fold is load bearing: sfxinfo_t::name is lower case and
     * wad.c's lump_name_eq() does a byte compare. */
    doom_dmx_lump_name("pistol", name);
    CU_ASSERT_STRING_EQUAL(name, "DSPISTOL");

    doom_dmx_lump_name("shotgn", name);
    CU_ASSERT_STRING_EQUAL(name, "DSSHOTGN");

    /* Shorter than 6 still terminates. */
    doom_dmx_lump_name("oof", name);
    CU_ASSERT_STRING_EQUAL(name, "DSOOF");

    /* Empty composes to the bare prefix -- which is why doom_dmx_find_sfx()
     * rejects an empty name rather than letting it reach the directory. */
    doom_dmx_lump_name("", name);
    CU_ASSERT_STRING_EQUAL(name, "DS");

    /* Longer than 6 is truncated to fill the 8-byte field exactly, and is
     * still NUL-terminated inside the 9-byte buffer. */
    doom_dmx_lump_name("abcdefghij", name);
    CU_ASSERT_STRING_EQUAL(name, "DSABCDEF");
    CU_ASSERT_EQUAL(name[8], '\0');

    /* Digits and already-upper characters pass through unchanged. */
    doom_dmx_lump_name("bg1sit", name);
    CU_ASSERT_STRING_EQUAL(name, "DSBG1SIT");
    doom_dmx_lump_name("PISTOL", name);
    CU_ASSERT_STRING_EQUAL(name, "DSPISTOL");
}

/* ---- u8 -> s16 conversion --------------------------------------------- */

static void test_to_s16_conversion(void)
{
    static const uint8_t body[5] = { 0, 127, 128, 129, 255 };
    doom_dmx_t dmx = { DOOM_DMX_FORMAT_PCM, 11025u, body, 5u };
    int16_t    dst[8];

    CU_ASSERT_EQUAL(doom_dmx_to_s16(&dmx, dst, 8), 5u);

    /* 128 is DMX's silence, and it must land on exactly 0 -- otherwise every
     * voice a mixer sums contributes its own DC offset. */
    CU_ASSERT_EQUAL(dst[2], 0);
    CU_ASSERT_EQUAL(dst[0], -32768);
    CU_ASSERT_EQUAL(dst[1], -256);
    CU_ASSERT_EQUAL(dst[3], 256);
    CU_ASSERT_EQUAL(dst[4], 32512);
}

static void test_to_s16_truncates_and_guards(void)
{
    static const uint8_t body[5] = { 0, 127, 128, 129, 255 };
    doom_dmx_t dmx = { DOOM_DMX_FORMAT_PCM, 11025u, body, 5u };
    int16_t    dst[5];

    /* A destination smaller than the sound writes what fits and says so,
     * rather than overrunning: the mixer fills fixed-size slices. */
    memset(dst, 0x5A, sizeof(dst));
    CU_ASSERT_EQUAL(doom_dmx_to_s16(&dmx, dst, 2), 2u);
    CU_ASSERT_EQUAL(dst[0], -32768);
    CU_ASSERT_EQUAL(dst[1], -256);
    CU_ASSERT_EQUAL(dst[2], 0x5A5A);   /* untouched */

    CU_ASSERT_EQUAL(doom_dmx_to_s16(&dmx, dst, 0), 0u);
    CU_ASSERT_EQUAL(doom_dmx_to_s16(&dmx, NULL, 5), 0u);
    CU_ASSERT_EQUAL(doom_dmx_to_s16(NULL, dst, 5), 0u);
}

/* ---- strerror ---------------------------------------------------------- */

static void test_strerror_covers_every_code(void)
{
    static const int codes[] = {
        DOOM_DMX_OK, DOOM_DMX_ENOENT, DOOM_DMX_EINVAL,
        DOOM_DMX_EFORMAT, DOOM_DMX_ETOOSHORT,
    };

    for (unsigned i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        const char *s = doom_dmx_strerror(codes[i]);
        CU_ASSERT_PTR_NOT_NULL(s);
        CU_ASSERT_STRING_NOT_EQUAL(s, "unknown error");
    }

    CU_ASSERT_STRING_EQUAL(doom_dmx_strerror(-99), "unknown error");
}

/* ---- the real freedoom2.wad ------------------------------------------- */

/*
 * The mounted WAD, or NULL.  Mounted once by the suite init rather than per
 * test: doom_wad_mount() re-parses the whole 3610-entry directory, and the
 * registry is process-global state shared with test_dg_init_k.c, so the
 * suite cleanup unmounts to leave it as it was found.
 */
static const wad_t *real_wad(void)
{
    const doom_wad_t *w = doom_wad_mounted();
    return w != NULL ? &w->wad : NULL;
}

int suite_doom_dmx_init(void)
{
    uint64_t start = 0, end = 0;

    if (mmap_find_module(&start, &end) != 0 || end <= start) {
        return 0;   /* no module; the tests below report it */
    }

    /* Identity-mapped by vmm_init() before run_tests() -- the same access
     * tests/kernel/test_vmm_fb_wad_k.c relies on. */
    (void)doom_wad_mount((const void *)(uintptr_t)start, (uint32_t)(end - start));
    return 0;
}

int suite_doom_dmx_cleanup(void)
{
    doom_wad_unmount();
    return 0;
}

static void test_real_wad_is_mounted(void)
{
    /* Not a tautology: if this fails, every assertion below is vacuous, so it
     * is worth failing here with a name that says why. */
    CU_ASSERT_PTR_NOT_NULL(real_wad());
}

static void test_real_dspistol_decodes(void)
{
    const wad_t *wad = real_wad();
    if (wad == NULL) return;

    uint32_t lump_size = 0;
    CU_ASSERT_PTR_NOT_NULL(wad_find_lump(wad, "DSPISTOL", &lump_size));
    CU_ASSERT_EQUAL(lump_size, PISTOL_LUMP_SIZE);

    doom_dmx_t dmx;
    CU_ASSERT_EQUAL(doom_dmx_find(wad, "DSPISTOL", &dmx), DOOM_DMX_OK);

    CU_ASSERT_EQUAL(dmx.format, DOOM_DMX_FORMAT_PCM);
    CU_ASSERT_EQUAL(dmx.rate_hz, PISTOL_RATE_HZ);
    CU_ASSERT_EQUAL(dmx.num_samples, PISTOL_SAMPLES);
    /* The header's own count, restated through the relationship rather than
     * read again, so the padding arithmetic is pinned from both ends. */
    CU_ASSERT_EQUAL(dmx.num_samples + 2u * DOOM_DMX_PAD_SAMPLES, PISTOL_COUNT);
    /* And the count accounts for the whole lump bar its header. */
    CU_ASSERT_EQUAL(PISTOL_COUNT + DOOM_DMX_HEADER_BYTES, PISTOL_LUMP_SIZE);

    CU_ASSERT_EQUAL(memcmp(dmx.samples, pistol_first8, 8), 0);
    CU_ASSERT_EQUAL(memcmp(dmx.samples + dmx.num_samples - 8, pistol_last8, 8), 0);
    CU_ASSERT_EQUAL(memcmp(dmx.samples + PISTOL_MID_OFFSET, pistol_mid8, 8), 0);

    /* The lead padding really was skipped: the raw body starts elsewhere. */
    const uint8_t *raw = dmx.samples - DOOM_DMX_PAD_SAMPLES;
    CU_ASSERT_EQUAL(raw[0], 145);
    CU_ASSERT_EQUAL(raw[1], 144);
    CU_ASSERT_EQUAL(raw[2], 141);
}

static void test_real_dsshotgn_decodes_at_its_own_rate(void)
{
    const wad_t *wad = real_wad();
    if (wad == NULL) return;

    doom_dmx_t dmx;
    CU_ASSERT_EQUAL(doom_dmx_find(wad, "DSSHOTGN", &dmx), DOOM_DMX_OK);

    /* Half DSPISTOL's rate, from the same WAD -- which is the whole reason
     * rate_hz is a field and not a constant. */
    CU_ASSERT_EQUAL(dmx.rate_hz, SHOTGN_RATE_HZ);
    CU_ASSERT_NOT_EQUAL(dmx.rate_hz, PISTOL_RATE_HZ);
    CU_ASSERT_EQUAL(dmx.num_samples, SHOTGN_SAMPLES);
    CU_ASSERT_EQUAL(dmx.num_samples + 2u * DOOM_DMX_PAD_SAMPLES, SHOTGN_COUNT);
    CU_ASSERT_EQUAL(memcmp(dmx.samples, shotgn_first4, 4), 0);
}

static void test_real_missing_lump_is_enoent(void)
{
    const wad_t *wad = real_wad();
    if (wad == NULL) return;

    doom_dmx_t dmx;
    CU_ASSERT_EQUAL(doom_dmx_find(wad, "DSZZZZZZ", &dmx), DOOM_DMX_ENOENT);
    CU_ASSERT_EQUAL(doom_dmx_find_sfx(wad, "zzzzzz", &dmx), DOOM_DMX_ENOENT);

    /* Lower case would find nothing even for a lump that exists, which is
     * what doom_dmx_lump_name()'s fold is there to prevent. */
    CU_ASSERT_EQUAL(doom_dmx_find(wad, "dspistol", &dmx), DOOM_DMX_ENOENT);

    CU_ASSERT_EQUAL(doom_dmx_find_sfx(wad, "", &dmx), DOOM_DMX_EINVAL);
    CU_ASSERT_EQUAL(doom_dmx_find_sfx(wad, NULL, &dmx), DOOM_DMX_EINVAL);
    CU_ASSERT_EQUAL(doom_dmx_find(NULL, "DSPISTOL", &dmx), DOOM_DMX_EINVAL);
}

/*
 * The sfxenum_t leg of the acceptance criterion.
 *
 * S_sfx[] is src/doom/sounds.c's table, which docker/scripts/build.sh
 * compiles only under TESTING -- the same narrow exception it already makes
 * for src/doom/tables.c, and the reason src/doom_dmx.c itself is name-based
 * rather than enum-based (it is globbed into shipped kernels, where S_sfx[]
 * does not exist).  Declared here rather than by including src/doom/sounds.h,
 * which would pull doomtype.h and the rest of the engine's headers into a
 * test TU compiled with -I src only.
 *
 * This mirrors sfxinfo_t's layout field for field (src/doom/i_sound.h), with
 * the two self-referential/opaque pointers as void * -- so it is only as good
 * as that copy, which is why the first assertion in the test reads names back
 * out of three known entries.  A layout that has drifted shows up there as
 * garbage rather than as a silently wrong sample.
 */
extern struct {
    char *tagname;
    char  name[9];
    int   priority;
    void *link;
    int   pitch;
    int   volume;
    int   usefulness;
    int   lumpnum;
    int   numchannels;
    void *driver_data;
} S_sfx[];

/* Ordinals from src/doom/sounds.h's sfxenum_t: sfx_None is 0 and index 0 is a
 * deliberate dummy ("needs to be a dummy for odd reasons", sounds.c), so the
 * first real effect is sfx_pistol at 1. */
#define SFX_NONE_ORDINAL   0
#define SFX_PISTOL_ORDINAL 1
#define SFX_SHOTGN_ORDINAL 2

static void test_real_sfxenum_resolves_to_its_lump(void)
{
    const wad_t *wad = real_wad();
    if (wad == NULL) return;

    /* Three entries, not one: together they catch both a reordered sfxenum_t
     * and a drifted sfxinfo_t layout above.  Either would decode the wrong
     * sound rather than fail, so neither is assumed. */
    CU_ASSERT_STRING_EQUAL(S_sfx[SFX_NONE_ORDINAL].name, "none");
    CU_ASSERT_STRING_EQUAL(S_sfx[SFX_PISTOL_ORDINAL].name, "pistol");
    CU_ASSERT_STRING_EQUAL(S_sfx[SFX_SHOTGN_ORDINAL].name, "shotgn");

    doom_dmx_t by_enum;
    CU_ASSERT_EQUAL(doom_dmx_find_sfx(wad, S_sfx[SFX_PISTOL_ORDINAL].name, &by_enum),
                    DOOM_DMX_OK);

    /* Identical to the name-based lookup, field for field. */
    doom_dmx_t by_name;
    CU_ASSERT_EQUAL(doom_dmx_find(wad, "DSPISTOL", &by_name), DOOM_DMX_OK);

    CU_ASSERT_EQUAL(by_enum.format, by_name.format);
    CU_ASSERT_EQUAL(by_enum.rate_hz, by_name.rate_hz);
    CU_ASSERT_EQUAL(by_enum.num_samples, by_name.num_samples);
    CU_ASSERT_TRUE(by_enum.samples == by_name.samples);
    CU_ASSERT_EQUAL(by_enum.num_samples, PISTOL_SAMPLES);
}

/*
 * Every DS lump in the directory, parsed.
 *
 * Two things this catches that DSPISTOL alone cannot: a lump in the DS
 * namespace this decoder chokes on, and -- via the rate histogram -- anyone
 * downstream assuming a single sample rate.  The exact counts are the point;
 * see this file's header comment on the SHA-1 pin.
 */
static void test_real_every_ds_lump_parses(void)
{
    const wad_t *wad = real_wad();
    if (wad == NULL) return;

    uint32_t seen = 0, ok = 0;
    uint32_t n11025 = 0, n16000 = 0, n17990 = 0, n22050 = 0, n44100 = 0, nother = 0;

    for (uint32_t i = 0; i < wad->numlumps; i++) {
        /* Walked directly: src/wad.h exposes no name iterator, and adding one
         * for a single test would be a wider change than this ticket needs.
         * 16-byte entries, name at +8, per wad_init(). */
        const uint8_t *entry = wad->dir_data + i * 16u;
        if (entry[8] != 'D' || entry[9] != 'S') {
            continue;
        }
        seen++;

        uint32_t       lump_size = 0;
        char           name[9];
        for (int j = 0; j < 8; j++) name[j] = (char)entry[8 + j];
        name[8] = '\0';

        const uint8_t *lump = wad_find_lump(wad, name, &lump_size);
        if (lump == NULL) continue;

        doom_dmx_t dmx;
        if (doom_dmx_parse(lump, lump_size, &dmx) != DOOM_DMX_OK) continue;
        ok++;

        switch (dmx.rate_hz) {
        case 11025: n11025++; break;
        case 16000: n16000++; break;
        case 17990: n17990++; break;
        case 22050: n22050++; break;
        case 44100: n44100++; break;
        default:    nother++; break;
        }
    }

    CU_ASSERT_EQUAL(seen, DS_LUMP_COUNT);
    CU_ASSERT_EQUAL(ok, DS_LUMP_COUNT);     /* not one of them is rejected */

    CU_ASSERT_EQUAL(n11025, DS_COUNT_11025);
    CU_ASSERT_EQUAL(n16000, DS_COUNT_16000);
    CU_ASSERT_EQUAL(n17990, DS_COUNT_17990);
    CU_ASSERT_EQUAL(n22050, DS_COUNT_22050);
    CU_ASSERT_EQUAL(n44100, DS_COUNT_44100);
    CU_ASSERT_EQUAL(nother, 0u);

    /* Stated as an assertion, not a comment: the majority rate is NOT the
     * 11025 Hz the ticket assumed, and it is not the HDA stream's 48 kHz
     * either. */
    CU_ASSERT_TRUE(n22050 > n11025);
}

void suite_doom_dmx_tests(CU_pSuite s)
{
    CU_add_test(s, "parse rejects NULL and sub-header lumps",
                test_parse_rejects_null_and_short);
    CU_add_test(s, "parse rejects a format tag other than 3",
                test_parse_rejects_wrong_format_tag);
    CU_add_test(s, "parse rejects a sample count past the lump",
                test_parse_rejects_count_past_lump);
    CU_add_test(s, "parse rejects a lump that is only padding",
                test_parse_rejects_padding_only_lump);
    CU_add_test(s, "parse leaves *out untouched on failure",
                test_parse_leaves_out_untouched_on_failure);
    CU_add_test(s, "parse strips 16 padding samples at each end",
                test_parse_strips_padding_at_both_ends);
    CU_add_test(s, "parse reports the recorded rate verbatim",
                test_parse_reports_rate_verbatim);
    CU_add_test(s, "sfx name composes to an upper-case DS lump name",
                test_lump_name_composition);
    CU_add_test(s, "u8 PCM converts to s16 centred on zero",
                test_to_s16_conversion);
    CU_add_test(s, "s16 conversion truncates to the destination",
                test_to_s16_truncates_and_guards);
    CU_add_test(s, "strerror names every result code",
                test_strerror_covers_every_code);

    CU_add_test(s, "the real freedoom2 module is mounted",
                test_real_wad_is_mounted);
    CU_add_test(s, "DSPISTOL decodes to its reference samples",
                test_real_dspistol_decodes);
    CU_add_test(s, "DSSHOTGN decodes at its own 11025 Hz rate",
                test_real_dsshotgn_decodes_at_its_own_rate);
    CU_add_test(s, "a missing or mis-cased lump is ENOENT",
                test_real_missing_lump_is_enoent);
    CU_add_test(s, "S_sfx[sfx_pistol] resolves to DSPISTOL",
                test_real_sfxenum_resolves_to_its_lump);
    CU_add_test(s, "every DS lump parses, at five different rates",
                test_real_every_ds_lump_parses);
}
