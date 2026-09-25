#include "doom_sfx_tone.h"
#include "doom/sounds.h"

#include <stddef.h>

/*
 * The table. Pitch conventions, so a new entry fits in:
 *   - explosions, big monsters, deaths: low (50-200 Hz), falling, long
 *   - small-arms fire: a short high click falling into a lower thump
 *   - pickups, switches, teleport: high and rising
 *   - doors/lifts: mid-low sweeps -- rising to open/start, falling to close
 *   - monster sight/pain: mid, with an up-down shape per species
 * Durations stay >= 15 ms: SCRUM-101 steps through these from Doom's game
 * loop, and much shorter than a frame would just be skipped.
 */

#define S1(f1, d1) \
    { 1, { { f1, d1 } } }
#define S2(f1, d1, f2, d2) \
    { 2, { { f1, d1 }, { f2, d2 } } }
#define S3(f1, d1, f2, d2, f3, d3) \
    { 3, { { f1, d1 }, { f2, d2 }, { f3, d3 } } }
#define S4(f1, d1, f2, d2, f3, d3, f4, d4) \
    { 4, { { f1, d1 }, { f2, d2 }, { f3, d3 }, { f4, d4 } } }

static const doom_sfx_tone_t sfx_tones[NUMSFX] = {
    /* ── Weapons ───────────────────────────────────────────────────── */
    [sfx_pistol] = S2(1200, 15,  600, 25),
    [sfx_shotgn] = S3( 900, 20,  300, 40,  150, 60),
    [sfx_sgcock] = S2(1800, 15, 1200, 15),
    [sfx_dshtgn] = S3( 800, 25,  250, 50,  120, 80),
    [sfx_dbopn]  = S2( 700, 30, 1000, 30),
    [sfx_dbcls]  = S2(1000, 30,  700, 30),
    [sfx_dbload] = S2(1400, 20, 1800, 20),
    [sfx_plasma] = S2(2000, 20, 2600, 20),
    [sfx_bfg]    = S4( 200, 80,  400, 80,  800, 80, 1600, 80),
    [sfx_sawup]  = S3( 150, 60,  250, 60,  350, 60),
    [sfx_sawidl] = S2( 180, 50,  160, 50),
    [sfx_sawful] = S3( 350, 40,  300, 40,  350, 40),
    [sfx_sawhit] = S3( 400, 40,  500, 40,  400, 40),
    [sfx_rlaunc] = S2( 300, 40,  200, 60),
    [sfx_rxplod] = S3( 120, 60,   90, 80,   60, 120),
    [sfx_firsht] = S2( 600, 40,  450, 50),
    [sfx_firxpl] = S2( 150, 50,  100, 70),
    [sfx_chgun]  = S2(1000, 15,  500, 20),
    [sfx_punch]  = S1( 220, 30),

    /* ── World: doors, lifts, switches ─────────────────────────────── */
    [sfx_pstart] = S2( 220, 60,  260, 60),
    [sfx_pstop]  = S2( 260, 50,  180, 70),
    [sfx_doropn] = S4( 250, 40,  350, 40,  450, 40,  550, 40),
    [sfx_dorcls] = S4( 550, 40,  450, 40,  350, 40,  250, 40),
    [sfx_bdopn]  = S2( 400, 40,  600, 40),
    [sfx_bdcls]  = S2( 600, 40,  400, 40),
    [sfx_stnmov] = S2( 110, 40,  130, 40),
    [sfx_swtchn] = S2(1500, 20,  900, 20),
    [sfx_swtchx] = S2( 900, 20, 1500, 20),
    [sfx_barexp] = S3( 150, 40,  100, 60,   70, 100),
    [sfx_metal]  = S2(1100, 30,  700, 30),
    [sfx_tink]   = S1(3000, 15),
    [sfx_noway]  = S1( 150, 80),
    [sfx_oof]    = S1( 160, 60),
    [sfx_telept] = S4( 400, 30,  800, 30, 1600, 30, 3200, 30),
    [sfx_radio]  = S2(1200, 30, 1400, 30),

    /* ── Pickups ───────────────────────────────────────────────────── */
    [sfx_itemup] = S2(1800, 25, 2400, 35),
    [sfx_wpnup]  = S3( 800, 30, 1200, 30, 1600, 40),
    [sfx_getpow] = S4( 600, 40,  800, 40, 1000, 40, 1200, 60),
    [sfx_itmbk]  = S3( 600, 30,  900, 30, 1200, 30),

    /* ── Player ────────────────────────────────────────────────────── */
    [sfx_plpain] = S2( 500, 40,  350, 60),
    [sfx_pldeth] = S4( 400, 80,  300, 80,  200, 80,  100, 120),
    [sfx_pdiehi] = S4( 500, 70,  350, 70,  250, 70,  150, 100),
    [sfx_slop]   = S2(  90, 60,   70, 80),

    /* ── Monster sight calls ───────────────────────────────────────── */
    [sfx_posit1] = S2( 350, 60,  450, 80),
    [sfx_posit2] = S2( 450, 60,  350, 80),
    [sfx_posit3] = S3( 300, 60,  400, 60,  300, 60),
    [sfx_bgsit1] = S3( 200, 50,  320, 50,  180, 90),   /* imp */
    [sfx_bgsit2] = S3( 220, 50,  160, 50,  280, 90),   /* imp */
    [sfx_sgtsit] = S2( 120, 80,  150, 80),
    [sfx_cacsit] = S2( 180, 70,  140, 90),
    [sfx_brssit] = S2( 100, 90,  140, 90),
    [sfx_cybsit] = S3(  80, 100, 110, 100,  80, 100),
    [sfx_spisit] = S2(  70, 120,  90, 120),
    [sfx_bspsit] = S3( 600, 40,  500, 40,  600, 40),
    [sfx_kntsit] = S2( 130, 80,  170, 80),
    [sfx_vilsit] = S3( 700, 60,  500, 60,  300, 80),
    [sfx_mansit] = S2( 110, 80,   90, 100),
    [sfx_pesit]  = S2( 250, 60,  200, 80),
    [sfx_sssit]  = S2( 350, 60,  450, 60),
    [sfx_skesit] = S2( 380, 50,  480, 50),
    [sfx_bossit] = S2( 100, 100, 150, 100),

    /* ── Monster pain ──────────────────────────────────────────────── */
    [sfx_dmpain] = S2( 280, 50,  220, 60),
    [sfx_popain] = S2( 400, 40,  300, 60),
    [sfx_vipain] = S2( 600, 50,  450, 60),
    [sfx_mnpain] = S2( 250, 50,  200, 60),
    [sfx_pepain] = S2( 450, 50,  350, 50),
    [sfx_keenpn] = S2( 600, 40,  500, 50),
    [sfx_bospn]  = S2( 220, 60,  180, 80),

    /* ── Monster attacks ───────────────────────────────────────────── */
    [sfx_sklatk] = S2( 900, 30,  700, 40),
    [sfx_sgtatk] = S2( 300, 30,  200, 40),
    [sfx_skepch] = S1( 250, 40),
    [sfx_vilatk] = S3( 150, 60,  300, 60,  600, 60),
    [sfx_claw]   = S2( 350, 30,  250, 30),
    [sfx_skeswg] = S2( 500, 30,  300, 30),
    [sfx_skeatk] = S2( 700, 30,  500, 30),
    [sfx_manatk] = S2( 400, 40,  300, 50),
    [sfx_flame]  = S3( 300, 40,  250, 40,  300, 40),
    [sfx_flamst] = S2( 200, 50,  300, 50),
    [sfx_bospit] = S2( 200, 60,  100, 80),
    [sfx_boscub] = S2( 300, 50,  400, 50),

    /* ── Monster deaths ────────────────────────────────────────────── */
    [sfx_podth1] = S2( 400, 60,  250, 90),
    [sfx_podth2] = S2( 350, 60,  220, 90),
    [sfx_podth3] = S3( 450, 50,  300, 50,  200, 80),
    [sfx_bgdth1] = S3( 300, 60,  200, 60,  120, 100),
    [sfx_bgdth2] = S2( 280, 60,  180, 100),
    [sfx_sgtdth] = S2( 150, 80,  100, 120),
    [sfx_cacdth] = S2( 200, 80,  120, 120),
    [sfx_skldth] = S2( 800, 40,  400, 60),
    [sfx_brsdth] = S2( 120, 100,  80, 140),
    [sfx_cybdth] = S3( 100, 120,  70, 160,  50, 200),
    [sfx_spidth] = S2(  90, 120,  60, 180),
    [sfx_bspdth] = S2( 500, 60,  300, 80),
    [sfx_vildth] = S3( 600, 60,  400, 80,  200, 100),
    [sfx_kntdth] = S2( 140, 90,   90, 130),
    [sfx_pedth]  = S2( 300, 60,  150, 100),
    [sfx_skedth] = S2( 400, 50,  250, 80),
    [sfx_mandth] = S2( 150, 80,  100, 120),
    [sfx_ssdth]  = S2( 400, 60,  250, 90),
    [sfx_keendt] = S2( 500, 60,  300, 90),
    [sfx_bosdth] = S3( 180, 100, 120, 120,  80, 160),

    /* ── Monster idle / movement ───────────────────────────────────── */
    [sfx_posact] = S1( 260, 60),
    [sfx_bgact]  = S1( 190, 70),
    [sfx_dmact]  = S1( 140, 70),
    [sfx_bspact] = S1( 550, 50),
    [sfx_bspwlk] = S1(  90, 30),
    [sfx_vilact] = S1( 480, 60),
    [sfx_skeact] = S1( 420, 50),
    [sfx_hoof]   = S1(  80, 40),
};

const doom_sfx_tone_t *doom_sfx_tone(int sfx_id)
{
    if (sfx_id <= sfx_None || sfx_id >= NUMSFX)
        return NULL;
    if (sfx_tones[sfx_id].n_steps == 0)
        return NULL;
    return &sfx_tones[sfx_id];
}

uint32_t doom_sfx_tone_total_ms(const doom_sfx_tone_t *t)
{
    uint32_t total = 0;

    if (t == NULL)
        return 0;
    for (uint8_t i = 0; i < t->n_steps && i < DOOM_SFX_MAX_STEPS; i++)
        total += t->step[i].dur_ms;
    return total;
}
