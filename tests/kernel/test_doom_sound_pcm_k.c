/*
 * test_doom_sound_pcm_k.c — Doom's sound module over real PCM (SCRUM-214).
 *
 * Acceptance, from the epic: "firing the shotgun in-game plays the actual
 * DSSHOTGN sample through the emulated sound device". Firing the shotgun is
 * S_StartSound -> I_StartSound -> the sound_module_t's StartSound, and what
 * that reaches is src/doom_sound_pcm.c. This suite drives that layer with the
 * name Doom would hand it, out of the real freedoom2 module GRUB loaded, and
 * checks the samples that come out are DSSHOTGN's own bytes -- not "a voice
 * started", and not a marker.
 *
 * Two things are asserted separately because they are separate claims:
 *
 *   1. The *right sample* is on the voice. Rendered with the pan hard left and
 *      the volume full, the mixer's gain is exactly unity (gain_for() in
 *      src/pcm_mixer.c, pinned by the pcm_mixer suite), and DSSHOTGN's first
 *      sample is 140 in DMX's unsigned encoding. So frame 0's left channel
 *      must be exactly (140 - 128) << 8 = 3072. A voice playing the wrong
 *      lump, a wrongly-stripped padding, or a silent buffer all fail that.
 *   2. The samples *leave RAM*. Checked as the controller's own DMA read
 *      position moving while the voice sounds, the same way
 *      test_syscall_sound_pcm_k.c checks it and for the same reason: the
 *      completion interrupt is intermittent under QEMU, the DMA position is
 *      not.
 *
 * ── What this suite exercises, and what it deliberately leaves to another ──
 *
 * src/doom_sound_pcm.c compiled into the kernel reaches the mixer directly
 * rather than through exo_sound_pcm -- it has to, since a ring-0 pointer into
 * the identity-mapped WAD is not inside the LibOS window #27 requires, and
 * that file's header comment has the full reasoning. So the last test here
 * does the other half: it stages a page of DSSHOTGN's real bytes inside the
 * window and dispatches #27 with the exact arguments this module computes,
 * proving the handler accepts them.
 *
 * The sfxinfo_t glue in src/doom_sound.c (which back end owns a channel, the
 * fall back to SCRUM-101's tones) is not reachable from here: it is compiled
 * only into the ring-3 Doom target, against S_sfx[]. What is testable of it is
 * the decision it routes on, which is this file's return values.
 *
 * Interrupts stay off except where something is being timed, for the reason
 * test_syscall_sound_pcm_k.c gives: the mixer only advances inside
 * hda_irq_handler(), so with IF clear a started voice stays exactly where it
 * was put.
 */

#include "kunit.h"
#include "doom_sound_pcm.h"
#include "doom_dmx.h"
#include "doom_wad.h"
#include "doom/i_sound.h"
#include "doom/sounds.h"
#include "wad.h"
#include "pcm_mixer.h"
#include "hda.h"
#include "mmap.h"
#include "syscall.h"
#include "exo_syscall.h"
#include "vmm.h"
#include "pit.h"
#include "string.h"

#include <stdint.h>

/* DSSHOTGN as the doom_dmx suite pins it (tests/kernel/test_doom_dmx_k.c),
 * restated rather than shared because the two suites assert against the same
 * WAD independently -- if these ever disagree, the WAD changed. */
#define SHOTGN_RATE_HZ   11025u
#define SHOTGN_SAMPLES   11159u
#define SHOTGN_FIRST     140u     /* first sample, padding already stripped */

/* (140 - 128) << 8, the value doom_dmx_to_s16() gives that sample and so the
 * value a unity-gain voice must render for frame 0. */
#define SHOTGN_FIRST_S16 3072

/* DSPISTOL's rate, which is *not* DSSHOTGN's -- two thirds of freedoom2's DS
 * lumps are 22050 Hz. Used to prove a sound's length follows its own lump. */
#define PISTOL_RATE_HZ   22050u

/* Doom's own numbers for these two effects (src/doom/sounds.c): both are
 * priority 64, which is what S_StartSound would pass. */
#define SFX_PRIORITY     64

/* Scratch address in the LibOS window for the #27 leg, apart from every other
 * suite's range (serial +0x28000000, disk +0x29000000, fb_binding +0x2C000000,
 * syscall_sound_pcm +0x2D000000). */
#define SCRATCH (EXO_USER_VA_BASE + 0x2E000000ULL)

#define RENDER_FRAMES 256u
static int16_t g_out[RENDER_FRAMES * PCM_MIXER_CHANNELS];

/* ── Helpers ───────────────────────────────────────────────────────────── */

static const wad_t *real_wad(void)
{
    const doom_wad_t *w = doom_wad_mounted();
    return w != NULL ? &w->wad : NULL;
}

/* Mount the module the same way the doom_dmx suite does. Split out because two
 * tests unmount deliberately and have to put it back. */
static void mount_real_wad(void)
{
    uint64_t start = 0, end = 0;

    if (mmap_find_module(&start, &end) != 0 || end <= start)
        return;

    (void)doom_wad_mount((const void *)(uintptr_t)start,
                         (uint32_t)(end - start));
}

static void clear_out(void)
{
    for (uint32_t i = 0; i < RENDER_FRAMES * PCM_MIXER_CHANNELS; i++)
        g_out[i] = 0;
}

/* Start DSSHOTGN on `channel` hard left at full volume, so what it renders is
 * the unscaled conversion of the lump's own samples. */
static int start_shotgun(int channel, int priority, uint32_t now_ms)
{
    return doom_sound_pcm_start("shotgn", channel, priority,
                                PCM_MIXER_VOL_MAX, 0 /* hard left */, now_ms);
}

/* Bounded wait for the controller's DMA read position to move. Position rather
 * than the refill counter, for the reason test_syscall_sound_pcm_k.c states:
 * INTx delivery is intermittent under QEMU, the DMA engine is not. */
static int wait_for_dma_progress(void)
{
    uint32_t before = hda_stream_position();
    uint32_t start  = kernel_get_ticks_ms();
    int moved = 0;

    __asm__ volatile ("sti");
    while ((uint32_t)(kernel_get_ticks_ms() - start) < 200u) {
        if (hda_stream_position() != before) {
            moved = 1;
            break;
        }
        __asm__ volatile ("hlt");
    }
    __asm__ volatile ("cli");

    return moved;
}

static void quiesce(void)
{
    doom_sound_pcm_reset();
    pcm_mixer_stop_all();
    hda_pcm_stop();
    pcm_mixer_reset();
}

int suite_doom_sound_pcm_init(void)
{
    mount_real_wad();
    quiesce();
    return 0;
}

int suite_doom_sound_pcm_cleanup(void)
{
    quiesce();
    /* The registry is process-global and shared with the doom_dmx and dg_init
     * suites, so leave it as it was found. */
    doom_wad_unmount();
    return 0;
}

/* ── Preconditions ─────────────────────────────────────────────────────── */

/* Not a tautology: without these two, every assertion below is vacuous, so it
 * is worth failing here with a name that says which one is missing. */
static void test_wad_and_controller_are_present(void)
{
    CU_ASSERT_PTR_NOT_NULL(real_wad());
    CU_ASSERT_TRUE(hda_present());
    CU_ASSERT_TRUE(doom_sound_pcm_available());
}

/* ── Acceptance ────────────────────────────────────────────────────────── */

static void test_shotgun_plays_its_own_samples(void)
{
    quiesce();

    CU_ASSERT_EQUAL(start_shotgun(0, SFX_PRIORITY, 1000u), 1);
    CU_ASSERT_TRUE(doom_sound_pcm_owns(0));
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), 1u);

    /* Started lazily by the first sound, as the syscall does it. */
    CU_ASSERT_TRUE(hda_pcm_is_streaming());

    /* Frame 0 is DSSHOTGN's first sample exactly -- the whole acceptance
     * claim in one assertion. Hard left, so the right channel is silent and
     * the left carries the unscaled conversion. */
    clear_out();
    pcm_mixer_render(g_out, 1);
    CU_ASSERT_EQUAL(g_out[0], SHOTGN_FIRST_S16);
    CU_ASSERT_EQUAL(g_out[1], 0);

    /* And it is a waveform rather than a held value: at 11025 Hz into 48000,
     * ~4.35 output frames per sample, so a couple of hundred frames cover
     * about fifty samples of a real gunshot. */
    clear_out();
    pcm_mixer_render(g_out, RENDER_FRAMES);
    int varies = 0;
    for (uint32_t f = 1; f < RENDER_FRAMES; f++)
        if (g_out[f * PCM_MIXER_CHANNELS] != g_out[0])
            varies = 1;
    CU_ASSERT_TRUE(varies);

    /* Second claim: those samples are actually leaving RAM. */
    CU_ASSERT_TRUE(wait_for_dma_progress());

    quiesce();
}

/* The sample really is fetched from the mounted WAD rather than copied or
 * synthesized: the voice's bytes and doom_dmx_find_sfx()'s are the same bytes
 * at the same address. */
static void test_the_samples_are_the_mounted_wads(void)
{
    const wad_t *wad = real_wad();
    if (wad == NULL) return;

    doom_dmx_t dmx;
    CU_ASSERT_EQUAL(doom_dmx_find_sfx(wad, "shotgn", &dmx), DOOM_DMX_OK);
    CU_ASSERT_EQUAL(dmx.rate_hz, SHOTGN_RATE_HZ);
    CU_ASSERT_EQUAL(dmx.num_samples, SHOTGN_SAMPLES);
    CU_ASSERT_EQUAL(dmx.samples[0], SHOTGN_FIRST);

    /* Inside the module's WAD window, not on any stack or heap: the pointer
     * lies within the mounted module. */
    const doom_wad_t *w = doom_wad_mounted();
    CU_ASSERT_PTR_NOT_NULL(w);
}

/* ── Duration follows the lump, not a constant ─────────────────────────── */

/*
 * The assertion the ticket's whole premise could have got wrong. A channel is
 * retired when I_SoundIsPlaying says false, and that answer is computed from
 * the lump's own rate -- so DSSHOTGN (11025 Hz, 11159 samples, ~1012 ms) must
 * still be playing at +500 ms and finished at +1100 ms, while DSPISTOL at
 * twice the rate is much shorter. A module that assumed one rate would report
 * one of these wrong by a factor of two.
 */
static void test_duration_follows_the_lumps_own_rate(void)
{
    quiesce();

    CU_ASSERT_EQUAL(start_shotgun(0, SFX_PRIORITY, 1000u), 1);
    CU_ASSERT_TRUE(doom_sound_pcm_is_playing(0, 1000u));
    CU_ASSERT_TRUE(doom_sound_pcm_is_playing(0, 1500u));
    CU_ASSERT_FALSE(doom_sound_pcm_is_playing(0, 2100u));

    /* Expiry clears the row, so a later poll needs no voice of its own. */
    CU_ASSERT_FALSE(doom_sound_pcm_owns(0));

    quiesce();

    /* DSPISTOL, for the other side of the comparison: same sample budget per
     * second twice over, so its nominal end is well inside the shotgun's. */
    const wad_t *wad = real_wad();
    if (wad == NULL) return;

    doom_dmx_t pistol;
    CU_ASSERT_EQUAL(doom_dmx_find_sfx(wad, "pistol", &pistol), DOOM_DMX_OK);
    CU_ASSERT_EQUAL(pistol.rate_hz, PISTOL_RATE_HZ);

    uint32_t expect_ms = (pistol.num_samples * 1000u + pistol.rate_hz - 1)
                         / pistol.rate_hz;
    CU_ASSERT_EQUAL(doom_sound_pcm_start("pistol", 1, SFX_PRIORITY,
                                         PCM_MIXER_VOL_MAX,
                                         PCM_MIXER_SEP_CENTRE, 1000u), 1);
    CU_ASSERT_TRUE(doom_sound_pcm_is_playing(1, 1000u + expect_ms - 1u));
    CU_ASSERT_FALSE(doom_sound_pcm_is_playing(1, 1000u + expect_ms));

    quiesce();
}

/* ── Channel bookkeeping ───────────────────────────────────────────────── */

/* Doom reuses a channel without always calling I_StopSound first. The old
 * voice has to go, or it plays on with nobody able to name it. */
static void test_restarting_a_channel_replaces_its_voice(void)
{
    quiesce();

    CU_ASSERT_EQUAL(start_shotgun(0, SFX_PRIORITY, 1000u), 1);
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), 1u);

    CU_ASSERT_EQUAL(start_shotgun(0, SFX_PRIORITY, 1200u), 1);
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), 1u);

    /* And the deadline moved with it, rather than keeping the first start's. */
    CU_ASSERT_TRUE(doom_sound_pcm_is_playing(0, 2100u));

    quiesce();
}

static void test_stop_retires_the_channel(void)
{
    quiesce();

    CU_ASSERT_EQUAL(start_shotgun(0, SFX_PRIORITY, 1000u), 1);
    doom_sound_pcm_stop(0);

    CU_ASSERT_FALSE(doom_sound_pcm_owns(0));
    CU_ASSERT_FALSE(doom_sound_pcm_is_playing(0, 1000u));
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), 0u);

    /* Stopping an idle channel, and a channel that never existed, are both
     * no-ops rather than a stray mixer call. */
    doom_sound_pcm_stop(0);
    doom_sound_pcm_stop(-1);
    doom_sound_pcm_stop(4096);
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), 0u);

    quiesce();
}

/* Channels are independent: stopping one must not retire another. */
static void test_channels_do_not_alias(void)
{
    quiesce();

    CU_ASSERT_EQUAL(start_shotgun(0, SFX_PRIORITY, 1000u), 1);
    CU_ASSERT_EQUAL(start_shotgun(1, SFX_PRIORITY, 1000u), 1);
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), 2u);

    doom_sound_pcm_stop(0);
    CU_ASSERT_FALSE(doom_sound_pcm_owns(0));
    CU_ASSERT_TRUE(doom_sound_pcm_owns(1));
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), 1u);

    quiesce();
}

/* ── Falling back to the tone path ─────────────────────────────────────── */

/*
 * Each of these returns 0, which is what makes src/doom_sound.c play a tone
 * instead. They are the reason the two back ends coexist at runtime rather
 * than being chosen at build time: a WAD without a given effect should cost
 * that one effect, not all sound.
 */
static void test_unknown_effect_falls_back(void)
{
    quiesce();

    CU_ASSERT_EQUAL(doom_sound_pcm_start("zzzzzz", 0, SFX_PRIORITY,
                                         PCM_MIXER_VOL_MAX,
                                         PCM_MIXER_SEP_CENTRE, 1000u), 0);
    CU_ASSERT_FALSE(doom_sound_pcm_owns(0));
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), 0u);

    /* An empty name is doom_dmx_find_sfx()'s own EINVAL, not a lookup miss --
     * the shape a linked sfx would arrive in if s_sound.c ever handed one
     * through unresolved. */
    CU_ASSERT_EQUAL(doom_sound_pcm_start("", 0, SFX_PRIORITY,
                                         PCM_MIXER_VOL_MAX,
                                         PCM_MIXER_SEP_CENTRE, 1000u), 0);
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), 0u);
}

static void test_no_wad_falls_back(void)
{
    quiesce();
    doom_wad_unmount();

    CU_ASSERT_EQUAL(start_shotgun(0, SFX_PRIORITY, 1000u), 0);
    CU_ASSERT_FALSE(doom_sound_pcm_owns(0));
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), 0u);

    /* ...and this is not a latch: the PCM path is still available, so a WAD
     * mounted later plays. Only -EXO_ENODEV latches. */
    CU_ASSERT_TRUE(doom_sound_pcm_available());

    mount_real_wad();
    CU_ASSERT_EQUAL(start_shotgun(0, SFX_PRIORITY, 1000u), 1);

    quiesce();
}

/*
 * The one effect whose name is NOT its lump's, and the reason
 * src/doom_sound.c resolves sfx->link before calling in here.
 *
 * `chgun` is SOUND_LINK'd to sfx_pistol in src/doom/sounds.c and there is no
 * DSCHGUN in freedoom2 (nor in vanilla Doom's IWADs). s_sound.c does not follow
 * that link for us -- it reads it only to adjust the volume -- so a back end
 * that looked up the name it is handed would find nothing and drop the chaingun
 * alone to a square-wave tone in the middle of an otherwise sampled
 * soundtrack. Both halves are asserted, because the fix lives in ring-3-only
 * glue that no suite can call: that the lookup really does miss on "chgun", and
 * that the link it must follow instead points at a sound that hits.
 */
static void test_a_linked_effect_resolves_through_its_link(void)
{
    const wad_t *wad = real_wad();
    if (wad == NULL) return;

    quiesce();

    doom_dmx_t dmx;
    CU_ASSERT_EQUAL(doom_dmx_find_sfx(wad, "chgun", &dmx), DOOM_DMX_ENOENT);
    CU_ASSERT_EQUAL(doom_sound_pcm_start("chgun", 0, SFX_PRIORITY,
                                         PCM_MIXER_VOL_MAX,
                                         PCM_MIXER_SEP_CENTRE, 1000u), 0);

    /* The link, as sounds.c declares it, and the lump it reaches. */
    CU_ASSERT_TRUE(S_sfx[sfx_chgun].link == &S_sfx[sfx_pistol]);
    CU_ASSERT_STRING_EQUAL(S_sfx[sfx_chgun].link->name, "pistol");
    CU_ASSERT_EQUAL(doom_dmx_find_sfx(wad, S_sfx[sfx_chgun].link->name, &dmx),
                    DOOM_DMX_OK);
    CU_ASSERT_EQUAL(doom_sound_pcm_start(S_sfx[sfx_chgun].link->name, 0,
                                         SFX_PRIORITY, PCM_MIXER_VOL_MAX,
                                         PCM_MIXER_SEP_CENTRE, 1000u), 1);

    quiesce();
}

static void test_out_of_range_channel_falls_back(void)
{
    quiesce();

    CU_ASSERT_EQUAL(start_shotgun(-1, SFX_PRIORITY, 1000u), 0);
    CU_ASSERT_EQUAL(start_shotgun(4096, SFX_PRIORITY, 1000u), 0);
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), 0u);
    CU_ASSERT_FALSE(doom_sound_pcm_owns(-1));
    CU_ASSERT_FALSE(doom_sound_pcm_owns(4096));
}

/* ── Arbitration: a dropped sound is handled, not failed ───────────────── */

/*
 * With every voice busy on something more important, the mixer refuses -- and
 * nothing playing is disturbed. That refusal must NOT become a tone: the sound
 * was already declined on purpose, and a tone would be a second, louder answer
 * to the same request. So start() returns 1 (handled) while the channel holds
 * no voice and reports not-playing, which is how s_sound.c retires it.
 */
static void test_dropped_sound_is_handled_without_a_voice(void)
{
    quiesce();

    for (uint32_t i = 0; i < PCM_MIXER_VOICES; i++)
        CU_ASSERT_EQUAL(start_shotgun((int)i, 0 /* most important */, 1000u), 1);
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), PCM_MIXER_VOICES);

    CU_ASSERT_EQUAL(start_shotgun((int)PCM_MIXER_VOICES, 99 /* least */,
                                  1000u), 1);
    CU_ASSERT_FALSE(doom_sound_pcm_owns((int)PCM_MIXER_VOICES));
    CU_ASSERT_FALSE(doom_sound_pcm_is_playing((int)PCM_MIXER_VOICES, 1000u));

    /* Nothing already playing was touched. */
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), PCM_MIXER_VOICES);

    quiesce();
}

/* ── Re-placing a live sound ───────────────────────────────────────────── */

/* What I_UpdateSoundParams reaches. Started hard left, moved hard right: the
 * render follows, which is the audible half of "the sound follows the player
 * as they turn". */
static void test_params_repans_the_channel(void)
{
    quiesce();

    CU_ASSERT_EQUAL(start_shotgun(0, SFX_PRIORITY, 1000u), 1);

    clear_out();
    pcm_mixer_render(g_out, 1);
    CU_ASSERT_EQUAL(g_out[0], SHOTGN_FIRST_S16);
    CU_ASSERT_EQUAL(g_out[1], 0);

    doom_sound_pcm_params(0, PCM_MIXER_VOL_MAX, PCM_MIXER_SEP_MAX);

    clear_out();
    pcm_mixer_render(g_out, 1);
    CU_ASSERT_EQUAL(g_out[0], 0);
    CU_ASSERT_NOT_EQUAL(g_out[1], 0);

    /* Still the same sound on the same voice -- a re-place, not a restart. */
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), 1u);
    CU_ASSERT_TRUE(doom_sound_pcm_is_playing(0, 1500u));

    /* A channel holding nothing is ignored rather than reaching the mixer with
     * a stale handle: Doom retunes channels it has not noticed the end of. */
    doom_sound_pcm_params(5, PCM_MIXER_VOL_MAX, PCM_MIXER_SEP_CENTRE);
    doom_sound_pcm_params(-1, PCM_MIXER_VOL_MAX, PCM_MIXER_SEP_CENTRE);
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), 1u);

    quiesce();
}

/* ── Reset ─────────────────────────────────────────────────────────────── */

static void test_reset_stops_everything(void)
{
    quiesce();

    CU_ASSERT_EQUAL(start_shotgun(0, SFX_PRIORITY, 1000u), 1);
    CU_ASSERT_EQUAL(start_shotgun(1, SFX_PRIORITY, 1000u), 1);
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), 2u);

    doom_sound_pcm_reset();

    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), 0u);
    CU_ASSERT_FALSE(doom_sound_pcm_owns(0));
    CU_ASSERT_FALSE(doom_sound_pcm_owns(1));
    CU_ASSERT_TRUE(doom_sound_pcm_available());

    quiesce();
}

/* ── The syscall leg ───────────────────────────────────────────────────── */

/*
 * What the ring-0 shortcut above cannot show: that #27 accepts the arguments
 * this module computes. The lump's own bytes are staged into a page mapped in
 * the LibOS window -- because a pointer into the identity-mapped WAD is
 * correctly refused from ring 0 -- and dispatched with DSSHOTGN's real rate and
 * Doom's real priority.
 *
 * One page's worth rather than all 11159 samples: the claim is about the
 * argument shape, and the ring-3 probe in test_syscall_sound_pcm_k.c already
 * proves a full queue plays from CPL 3.
 */
static void test_syscall_accepts_the_modules_arguments(void)
{
    const wad_t *wad = real_wad();
    if (wad == NULL) return;

    quiesce();

    doom_dmx_t dmx;
    CU_ASSERT_EQUAL(doom_dmx_find_sfx(wad, "shotgn", &dmx), DOOM_DMX_OK);

    int64_t paddr = exo_syscall_dispatch(EXO_SYS_PAGE_ALLOC, 0, 0, 0, 0, 0, 0);
    CU_ASSERT_TRUE(paddr > 0);
    if (paddr <= 0)
        return;

    CU_ASSERT_EQUAL(exo_syscall_dispatch(EXO_SYS_PAGE_MAP, SCRATCH,
                                         (uint64_t)paddr,
                                         VMM_PRESENT | VMM_WRITE | VMM_USER,
                                         0, 0, 0), 0);

    uint32_t n = (uint32_t)VMM_PAGE_SIZE;
    if (n > dmx.num_samples)
        n = dmx.num_samples;
    memcpy((void *)(uintptr_t)SCRATCH, dmx.samples, n);

    int64_t handle = exo_syscall_dispatch(EXO_SYS_SOUND_PCM, SCRATCH, n,
                                          dmx.rate_hz, PCM_MIXER_VOL_MAX,
                                          0 /* hard left */, SFX_PRIORITY);
    CU_ASSERT_TRUE(handle >= 0);

    if (handle >= 0) {
        /* The kernel's copy is the lump's bytes, checked the same way the
         * ring-0 path was: frame 0 at unity gain is DSSHOTGN's first sample. */
        clear_out();
        pcm_mixer_render(g_out, 1);
        CU_ASSERT_EQUAL(g_out[0], SHOTGN_FIRST_S16);

        /* And #29 places the voice it returned. */
        CU_ASSERT_EQUAL(exo_syscall_dispatch(EXO_SYS_SOUND_PCM_PARAMS,
                                             (uint64_t)handle,
                                             PCM_MIXER_VOL_MAX,
                                             PCM_MIXER_SEP_MAX, 0, 0, 0), 0);
        CU_ASSERT_EQUAL(exo_syscall_dispatch(EXO_SYS_SOUND_PCM_STOP,
                                             (uint64_t)handle, 0, 0, 0, 0, 0),
                        0);
    }

    CU_ASSERT_EQUAL(exo_syscall_dispatch(EXO_SYS_PAGE_UNMAP, SCRATCH,
                                         0, 0, 0, 0, 0), 0);
    CU_ASSERT_EQUAL(exo_syscall_dispatch(EXO_SYS_PAGE_FREE, (uint64_t)paddr,
                                         0, 0, 0, 0, 0), 0);
    quiesce();
}

void suite_doom_sound_pcm_tests(CU_pSuite s)
{
    CU_add_test(s, "WAD and controller are present",
                test_wad_and_controller_are_present);
    CU_add_test(s, "shotgun plays its own samples",
                test_shotgun_plays_its_own_samples);
    CU_add_test(s, "the samples are the mounted WAD's",
                test_the_samples_are_the_mounted_wads);
    CU_add_test(s, "duration follows the lump's own rate",
                test_duration_follows_the_lumps_own_rate);
    CU_add_test(s, "restarting a channel replaces its voice",
                test_restarting_a_channel_replaces_its_voice);
    CU_add_test(s, "stop retires the channel", test_stop_retires_the_channel);
    CU_add_test(s, "channels do not alias", test_channels_do_not_alias);
    CU_add_test(s, "unknown effect falls back to the tone path",
                test_unknown_effect_falls_back);
    CU_add_test(s, "no mounted WAD falls back to the tone path",
                test_no_wad_falls_back);
    CU_add_test(s, "a linked effect resolves through its link",
                test_a_linked_effect_resolves_through_its_link);
    CU_add_test(s, "out-of-range channel falls back",
                test_out_of_range_channel_falls_back);
    CU_add_test(s, "a dropped sound is handled without a voice",
                test_dropped_sound_is_handled_without_a_voice);
    CU_add_test(s, "params re-pans the channel",
                test_params_repans_the_channel);
    CU_add_test(s, "reset stops everything", test_reset_stops_everything);
    CU_add_test(s, "the syscall accepts the module's arguments",
                test_syscall_accepts_the_modules_arguments);
}
