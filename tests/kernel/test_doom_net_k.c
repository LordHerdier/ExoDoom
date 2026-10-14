/*
 * test_doom_net_k.c — Doom's net layer with the network stubbed out
 * (SCRUM-97).
 *
 * Acceptance: "Single-player works cleanly; no crashes from uninitialized
 * net state."
 *
 * ── What is actually under test ───────────────────────────────────────
 *
 * Doom has no separate single-player loop. Every tic of every game goes
 * through the code that drives a netgame: d_loop.c samples input into a
 * ticcmd, files it in a ring of BACKUPTICS slots, works out from the clock
 * how many tics are due, and hands each one to the game with a mask of who
 * is playing. "Single player" is that machinery with nobody else connected.
 *
 * ExoDoom vendors those two files (src/doom/d_loop.c, src/doom/d_net.c) but
 * none of the networking beneath them -- net_client.c, net_server.c and the
 * rest were left out by SCRUM-63 -- and stands in for the whole missing
 * layer with two booleans in src/doom_net_stub.c. So the thing that can go
 * wrong is not "the network code crashes"; there isn't any. It is the state
 * the loop reads on its way past where the network would have been: fields
 * a server was meant to fill, statics a connect path was meant to assign,
 * a divisor that is zero until somebody sets it.
 *
 * This suite links the REAL d_loop.c and d_net.c into the test kernel
 * (docker/scripts/build.sh step 3d, the same narrow exception SCRUM-41 made
 * for tables.c) and runs them the way D_DoomMain and D_DoomLoop do, with
 * only the game engine below them faked: D_ConnectNetGame, D_CheckNetGame,
 * the one TryRunTics D_DoomLoop makes before the loop clock starts,
 * D_StartGameLoop, then a tic at a time. Nothing here is a re-implementation
 * of the loop -- if a re-vendor changes how it behaves, this is where that
 * shows.
 *
 * ── Why the tests are one continuous session ──────────────────────────
 *
 * d_loop.c keeps its state in file statics (maketic, lasttime, the ticdata
 * ring, TryRunTics's own `oldentertics`) with no reset entry point, because
 * a real Doom process initialises once. The cases below therefore run in
 * registration order as a single boot rather than each starting clean, and
 * each one asserts on the stage it adds. KUnit runs tests in the order they
 * are added, so that order is the order of the file.
 *
 * ── The fake engine ───────────────────────────────────────────────────
 *
 * d_net.c is glue: it copies game settings in and out of globals that live
 * in g_game.c/d_main.c, and its RunTic calls G_Ticker. Those files are the
 * engine proper and cannot link into a kernel image, so this TU defines the
 * 22 globals and 16 functions `nm -u` lists for the two objects. They are
 * declared through Doom's own headers rather than restated, so a type that
 * drifts from the engine's is a compile error here, not a silent ABI
 * mismatch (boolean is a 4-byte unsigned in this tree -- doomtype.h -- and
 * three of the structures crossing this boundary are arrays of it).
 *
 * The fakes record rather than simulate. G_BuildTiccmd stamps each command
 * with the tic it was built for; G_Ticker checks the command it is handed
 * carries the stamp of the tic being run. That one comparison is what turns
 * "it did not crash" into "it ran the right input": a ring slot replayed
 * from a previous lap, a tic run before it was built, or a slot for a player
 * who is not here all fail it.
 */

#include "kunit.h"

#include "doom/doomstat.h"
#include "doom/d_loop.h"
#include "doom/d_main.h"
#include "doom/g_game.h"
#include "doom/i_system.h"
#include "doom/i_timer.h"
#include "doom/i_video.h"
#include "doom/m_argv.h"
#include "doom/m_menu.h"
#include "doom/m_misc.h"
#include "doom/net_client.h"
#include "doom/w_checksum.h"
#include "doom/w_wad.h"

#include <stddef.h>
#include <stdint.h>

/* d_net.c's two entry points. Upstream declares them inside d_main.c
 * (src/doom/d_main.c:131) rather than in a header, so there is nothing to
 * include for them. */
void D_ConnectNetGame(void);
void D_CheckNetGame(void);

/* ---- Game state d_net.c reads and writes --------------------------- */

player_t      players[MAXPLAYERS];
boolean       playeringame[MAXPLAYERS];
int           consoleplayer;
boolean       netgame;
int           deathmatch;
boolean       nomonsters;
boolean       respawnparm;
boolean       fastparm;
GameMode_t    gamemode;
GameMission_t gamemission;
GameVersion_t gameversion;
skill_t       startskill;
int           startepisode;
int           startmap;
int           startloadgame;
boolean       autostart;
int           timelimit;
boolean       lowres_turn;
boolean       demoplayback;
boolean       demorecording;
boolean       advancedemo;
int           viewangleoffset;

/* ---- A clock the tests move by hand -------------------------------- */

/*
 * Milliseconds, like DG_GetTicksMs(). Both of Doom's clock readers derive
 * from it with the same arithmetic src/doom/i_timer.c uses, so d_loop.c's
 * two views of time (GetAdjustedTime() from the ms reading, TryRunTics()
 * from the tic reading) agree here exactly as they do at runtime.
 *
 * I_Sleep advances it. TryRunTics waits for the next tic by sleeping a
 * millisecond at a time, and with interrupts off under TESTING a real sleep
 * would never return (see docs/architecture.md on DG_SleepMs) -- so the fake
 * is what makes that wait loop terminate, and makes it deterministic.
 */
static uint32_t clock_ms;

int I_GetTimeMS(void)
{
    return (int)clock_ms;
}

int I_GetTime(void)
{
    return (int)((clock_ms * TICRATE) / 1000);
}

void I_Sleep(int ms)
{
    clock_ms += (uint32_t)ms;
}

/* The first millisecond at which I_GetTime() reads exactly `tic`. */
static void clock_set_tic(int tic)
{
    clock_ms = ((uint32_t)tic * 1000 + (TICRATE - 1)) / TICRATE;
}

/* ---- Recorders ------------------------------------------------------ */

static int           n_errors;
static const char   *last_error;
static int           n_atexit;
static atexit_func_t atexit_func;
static boolean       atexit_on_error;
static int           n_checksum;
static int           n_start_tic;
static int           n_process_events;
static int           n_run_menu;
static int           n_build;
static int           n_ticker;
static int           n_advance_demo;

/* What G_Ticker found wrong with the tics it was handed. All three stay at
 * zero in a correct run; see G_Ticker below for what each one means. */
static int           n_no_netcmds;
static int           n_wrong_cmd;
static int           n_foreign_cmd;

/*
 * I_Error does not return in a real Doom. Here it has to -- there is no
 * process to end and the suite must reach its assertions -- so every caller
 * carries on into a state the engine considers impossible. That is fine for
 * what it is used for: every case asserts the count is still zero.
 */
void I_Error(char *error, ...)
{
    n_errors++;
    last_error = error;
}

void I_AtExit(atexit_func_t func, boolean run_if_error)
{
    n_atexit++;
    atexit_func = func;
    atexit_on_error = run_if_error;
}

void I_StartTic(void)
{
    n_start_tic++;
}

/*
 * libos_doom.c hands Doom argv = { "exodoom" } and nothing else, so on this
 * port M_CheckParm() answers 0 to every question: no -server, no -connect,
 * no -solo-net, no -left/-right. That is the configuration under test.
 */
int M_CheckParm(char *check)
{
    (void)check;
    return 0;
}

boolean M_StringCopy(char *dest, const char *src, size_t dest_size)
{
    size_t i;

    if (dest_size == 0) {
        return false;
    }
    for (i = 0; i + 1 < dest_size && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    dest[i] = '\0';
    return src[i] == '\0';
}

/* The real one hashes the WAD directory. What matters to the net layer is
 * only that the digest is written, so the caller is not left holding 20
 * bytes of whatever was on its stack. */
void W_Checksum(sha1_digest_t digest)
{
    memset(digest, 0x5A, sizeof(sha1_digest_t));
    n_checksum++;
}

int W_CheckNumForName(char *name)
{
    (void)name;
    return -1;
}

void D_ProcessEvents(void)
{
    n_process_events++;
}

void M_Ticker(void)
{
    n_run_menu++;
}

void D_DoAdvanceDemo(void)
{
    n_advance_demo++;
    advancedemo = false;
}

boolean G_CheckDemoStatus(void)
{
    return false;
}

/*
 * Stamp the command with the tic it was built for. `inventory` is the one
 * full-width field (Strife's; Doom never touches it, and neither does
 * d_loop.c's TicdupSquash), so it carries the whole tic number rather than
 * a byte of it. forwardmove = 1 marks the slot as having been built at all,
 * which a zeroed, never-built slot would not have.
 */
void G_BuildTiccmd(ticcmd_t *cmd, int maketic)
{
    cmd->forwardmove = 1;
    cmd->inventory = maketic;
    n_build++;
}

/*
 * Called by d_net.c's RunTic for the tic numbered `gametic` (d_loop.c
 * increments it only after this returns).
 *
 *   n_no_netcmds    netcmds was still NULL -- a tic ran without RunTic
 *                   having pointed the game at its input.
 *   n_wrong_cmd     player 0's command is not the one built for this tic:
 *                   never built (forwardmove still 0), or built for another
 *                   tic and reached through a stale or replayed ring slot.
 *   n_foreign_cmd   a slot for a player who is not here holds something.
 *                   Nobody ever builds one, so with the network absent
 *                   these must still be the zeroes .bss gave them. Checked
 *                   across all NET_MAXPLAYERS slots, not just Doom's four:
 *                   the ring is sized for 8 and netcmds points into it.
 */
void G_Ticker(void)
{
    static const ticcmd_t no_input;
    int i;

    n_ticker++;

    if (netcmds == NULL) {
        n_no_netcmds++;
        return;
    }

    if (netcmds[0].forwardmove != 1 || netcmds[0].inventory != gametic) {
        n_wrong_cmd++;
    }

    for (i = 1; i < NET_MAXPLAYERS; i++) {
        if (memcmp(&netcmds[i], &no_input, sizeof(ticcmd_t)) != 0) {
            n_foreign_cmd++;
        }
    }
}

/* ---- Helpers --------------------------------------------------------- */

/*
 * Overwrite the stack below the caller, so that whatever the next call
 * leaves uninitialised in its own frame reads back as `fill` rather than as
 * the leftovers of some earlier test -- which could be zero by luck.
 * volatile so the stores are not optimised away; noinline so they land in a
 * frame of their own, below the caller's, where the next callee's will be.
 */
static __attribute__((noinline)) void poison_stack(uint8_t fill)
{
    volatile uint8_t pit[2048];
    size_t i;

    for (i = 0; i < sizeof(pit); i++) {
        pit[i] = fill;
    }
}

/* TryRunTics, returning how many tics of game it ran. */
static int run_frame(void)
{
    int before = n_ticker;

    TryRunTics();
    return n_ticker - before;
}

/* Everything the loop is meant to leave alone, in one place. */
static void assert_single_player_intact(void)
{
    CU_ASSERT_EQUAL(n_errors, 0);
    CU_ASSERT_EQUAL(n_no_netcmds, 0);
    CU_ASSERT_EQUAL(n_wrong_cmd, 0);
    CU_ASSERT_EQUAL(n_foreign_cmd, 0);
    CU_ASSERT_EQUAL(n_ticker, gametic);

    CU_ASSERT_EQUAL(consoleplayer, 0);
    CU_ASSERT_TRUE(playeringame[0]);
    CU_ASSERT_FALSE(playeringame[1]);
    CU_ASSERT_FALSE(playeringame[2]);
    CU_ASSERT_FALSE(playeringame[3]);
    CU_ASSERT_FALSE(netgame);
    CU_ASSERT_EQUAL(ticdup, 1);
}

/* ---- The session ----------------------------------------------------- */

/*
 * The two symbols src/doom_net_stub.c supplies in place of net_client.c.
 *
 * `drone` is the one everything below depends on. For a drone, BuildNewTic
 * builds no ticcmd and PlayersInGame counts nobody, so TryRunTics would
 * never run a single tic -- a silent freeze, with no error to name the
 * cause. `net_client_connected` is what keeps TryRunTics on
 * SinglePlayerClear and off OldNetSync, which paces the loop against
 * `recvtic`, a counter only D_ReceiveTic advances and nothing here calls.
 */
static void test_stub_describes_an_absent_network(void)
{
    CU_ASSERT_FALSE(net_client_connected);
    CU_ASSERT_FALSE(drone);
}

/*
 * D_ConnectNetGame is where a real Doom would find its server. With no
 * network it must conclude "not a netgame" on its own, not inherit whatever
 * `netgame` held, and still register its disconnect hook -- D_QuitNetGame is
 * on I_Error's atexit list (run_if_error) so a crashing client lets go of
 * the server, and that list is walked by this port's I_Error too.
 */
static void test_connect_finds_no_netgame(void)
{
    netgame = true;

    D_ConnectNetGame();

    CU_ASSERT_FALSE(netgame);
    CU_ASSERT_EQUAL(n_atexit, 1);
    CU_ASSERT(atexit_func == D_QuitNetGame);
    CU_ASSERT_TRUE(atexit_on_error);
    CU_ASSERT_EQUAL(n_checksum, 1);
    CU_ASSERT_EQUAL(viewangleoffset, 0);
    CU_ASSERT_EQUAL(n_errors, 0);
}

/*
 * The uninitialised read this ticket found.
 *
 * D_ConnectNetGame keeps a net_connect_data_t on its stack and
 * InitConnectData fills it -- except `player_class`, which is Hexen's and
 * which no Doom code assigns, and `deh_sha1sum`, whose only writer sits
 * under `#if ORIGCODE`. D_InitNetGame then reads connect_data->player_class
 * unconditionally and parks it in a d_loop.c static, and D_StartNetGame
 * copies that static into settings->player_classes[0] every time it runs.
 *
 * So the value handed back below is the one D_ConnectNetGame read off its
 * own stack. Poisoned first, so "it happened to be zero" cannot pass: before
 * the fix in src/doom/d_net.c this returned 0xA5A5A5A5 and then 0x5A5A5A5A,
 * i.e. whatever the previous call left at that depth. On a real boot that
 * is D_DoomMain's own earlier calls, so it was deterministic garbage rather
 * than a crash -- Doom never consumes a player class -- but it is net state
 * a single-player boot read uninitialised, and it is the first field a
 * re-enabled network layer would have put on the wire.
 */
static void test_connect_data_carries_no_stack_garbage(void)
{
    net_gamesettings_t settings;

    poison_stack(0xA5);
    D_ConnectNetGame();
    D_StartNetGame(&settings, NULL);
    CU_ASSERT_EQUAL(settings.player_classes[0], 0);

    poison_stack(0x5A);
    D_ConnectNetGame();
    D_StartNetGame(&settings, NULL);
    CU_ASSERT_EQUAL(settings.player_classes[0], 0);

    CU_ASSERT_EQUAL(n_errors, 0);
}

/*
 * D_StartNetGame is the half a server would have answered. With ORIGCODE
 * off it invents the reply: one player, at console 0, vanilla sync, no tic
 * duplication.
 *
 * ticdup matters more than it looks. It is a plain zero-initialised global
 * and d_loop.c divides by it in ten places (GetAdjustedTime()/ticdup in
 * NetUpdate and D_StartGameLoop, gametic/ticdup and I_GetTime()/ticdup all
 * through BuildNewTic and TryRunTics), so the tic loop is a #DE waiting to
 * happen until this call has run. D_DoomMain calls D_CheckNetGame -- and
 * through it this -- before D_DoomLoop, and nothing ahead of that reaches
 * NetUpdate; the assertion here pins the value that ordering depends on.
 */
static void test_start_netgame_invents_one_local_player(void)
{
    net_gamesettings_t settings;

    memset(&settings, 0xFF, sizeof(settings));
    ticdup = 0;

    D_StartNetGame(&settings, NULL);

    CU_ASSERT_EQUAL(settings.consoleplayer, 0);
    CU_ASSERT_EQUAL(settings.num_players, 1);
    CU_ASSERT_EQUAL(settings.ticdup, 1);
    CU_ASSERT_EQUAL(settings.extratics, 1);
    CU_ASSERT_EQUAL(settings.new_sync, 0);
    CU_ASSERT_EQUAL(ticdup, 1);
}

/*
 * D_CheckNetGame round-trips the game's start parameters through that
 * invented reply and installs the result. Seeded with a player table that
 * is wrong in every slot, to show the table comes out of the settings and
 * not out of whatever was there: console 0, player 0 in, nobody else.
 * What the user asked for (skill, episode, map) must survive the trip.
 */
static void test_check_netgame_settles_the_player_table(void)
{
    consoleplayer   = 3;
    playeringame[0] = false;
    playeringame[1] = true;
    playeringame[2] = true;
    playeringame[3] = true;

    startskill    = sk_hard;
    startepisode  = 2;
    startmap      = 7;
    startloadgame = -1;
    deathmatch    = 0;
    timelimit     = 0;
    autostart     = false;

    D_CheckNetGame();

    CU_ASSERT_EQUAL(consoleplayer, 0);
    CU_ASSERT_TRUE(playeringame[0]);
    CU_ASSERT_FALSE(playeringame[1]);
    CU_ASSERT_FALSE(playeringame[2]);
    CU_ASSERT_FALSE(playeringame[3]);

    CU_ASSERT_EQUAL(startskill, sk_hard);
    CU_ASSERT_EQUAL(startepisode, 2);
    CU_ASSERT_EQUAL(startmap, 7);
    CU_ASSERT_EQUAL(startloadgame, -1);
    CU_ASSERT_EQUAL(deathmatch, 0);
    CU_ASSERT_EQUAL(timelimit, 0);

    /* Not a netgame, so nothing may force an autostart past the title. */
    CU_ASSERT_FALSE(netgame);
    CU_ASSERT_FALSE(autostart);
    CU_ASSERT_FALSE(lowres_turn);
    CU_ASSERT_EQUAL(ticdup, 1);
    CU_ASSERT_EQUAL(n_errors, 0);
}

/*
 * D_DoomLoop calls TryRunTics once BEFORE D_StartGameLoop has given the
 * loop its starting time (src/doom/d_main.c:440 vs :451), so that first
 * call sees `lasttime` as the zero .bss left it and believes every tic
 * since the timer started is owed at once. Two seconds of startup is 70.
 *
 * What keeps that from being 70 tics of game run in one frame -- or from
 * running tics nobody built -- is the build-ahead cap in BuildNewTic: five
 * commands and no more. So exactly five tics run, each on its own command.
 */
static void test_first_frame_before_the_loop_clock_starts(void)
{
    clock_set_tic(70);

    CU_ASSERT_EQUAL(run_frame(), 5);
    CU_ASSERT_EQUAL(gametic, 5);
    CU_ASSERT_EQUAL(n_build, 5);

    D_StartGameLoop();

    assert_single_player_intact();
}

/*
 * The steady state: one tic of wall time, one tic of game, for ten seconds.
 * Each frame's bookkeeping is checked as it happens, so a frame that ran
 * two tics and a frame that ran none cannot cancel out.
 */
static void test_one_tic_of_time_runs_one_tic_of_game(void)
{
    int tic;
    int wrong_frames = 0;

    for (tic = 71; tic < 71 + 10 * TICRATE; tic++) {
        clock_set_tic(tic);
        if (run_frame() != 1) {
            wrong_frames++;
        }
    }

    CU_ASSERT_EQUAL(wrong_frames, 0);
    CU_ASSERT_EQUAL(gametic, 5 + 10 * TICRATE);

    /* The loop polls input and runs the menu every time it builds, and once
     * more on each attempt the cap turns away. */
    CU_ASSERT_TRUE(n_start_tic >= n_build);
    CU_ASSERT_EQUAL(n_process_events, n_start_tic);
    CU_ASSERT_EQUAL(n_run_menu, n_start_tic);

    assert_single_player_intact();
}

/*
 * A stall -- a ten-second hitch, the machine paused under a debugger --
 * must cost the game time, not correctness. The loop may only run tics it
 * has built, and it may build at most five ahead; so 350 owed tics become
 * five, the rest are dropped, and the frame after resumes one for one.
 * The alternative failure here is I_Error("gametic>lowtic") or
 * ("TryRunTics: lowtic < gametic"), which is what running past the built
 * tics looks like from inside.
 */
static void test_stall_drops_time_rather_than_inventing_tics(void)
{
    int tic = I_GetTime();
    int before = gametic;

    clock_set_tic(tic + 10 * TICRATE);
    CU_ASSERT_EQUAL(run_frame(), 5);

    clock_set_tic(tic + 10 * TICRATE + 1);
    CU_ASSERT_EQUAL(run_frame(), 1);

    CU_ASSERT_EQUAL(gametic, before + 6);
    assert_single_player_intact();
}

/*
 * A player the game believes in but the network never supplies.
 *
 * playeringame[] can outlive whatever set it: a multiplayer demo sets
 * entries 1-3 from the demo header, and a savegame restores all four. In a
 * real netgame the server's ingame mask is what retires them. Here the mask
 * comes out of the tic ring, where BuildNewTic marks the local player's slot
 * and nobody else's, and SinglePlayerClear re-clears the rest every tic --
 * so the phantom is retired on the very next tic, through the same
 * PlayerQuitGame path a real disconnect takes, and the game is told.
 *
 * The alternative would be G_Ticker running a player on a ticcmd nobody
 * built, every tic, for as long as the entry stayed set.
 */
static void test_phantom_player_is_retired_not_simulated(void)
{
    int tic = I_GetTime();

    playeringame[2] = true;

    clock_set_tic(tic + 1);
    CU_ASSERT_EQUAL(run_frame(), 1);

    CU_ASSERT_FALSE(playeringame[2]);
    CU_ASSERT_PTR_NOT_NULL(players[0].message);
    if (players[0].message != NULL) {
        CU_ASSERT_STRING_EQUAL(players[0].message, "Player 3 left the game");
    }
    players[0].message = NULL;

    assert_single_player_intact();
}

/*
 * The same loop while a demo plays, which is what the title screen moves on
 * to by itself after eleven seconds. demoplayback suppresses the quit check, so
 * a multiplayer demo's extra players are left alone for G_Ticker to feed
 * from the demo lump; the net loop must keep supplying exactly one command
 * a tic regardless and must not retire them underneath the demo.
 */
static void test_demo_playback_leaves_demo_players_alone(void)
{
    int tic = I_GetTime();
    int i;

    demoplayback    = true;
    playeringame[1] = true;

    for (i = 1; i <= TICRATE; i++) {
        clock_set_tic(tic + i);
        CU_ASSERT_EQUAL(run_frame(), 1);
    }

    CU_ASSERT_TRUE(playeringame[1]);
    CU_ASSERT_PTR_NULL(players[0].message);

    /* What G_CheckDemoStatus does when the demo ends. */
    demoplayback    = false;
    playeringame[1] = false;

    assert_single_player_intact();
}

/*
 * Twenty thousand frames at whatever pace an LCG dictates: frames that
 * arrive early (the loop waits, then returns without running anything),
 * on time, and several tics late. The ticdata ring is 128 slots, so this
 * laps it well over a hundred times; G_Ticker's stamp check is what would
 * notice a slot from a previous lap being run again.
 */
static void test_long_session_at_an_uneven_frame_rate(void)
{
    uint32_t seed = 0x5C2A97u;
    int start = gametic;
    int frame;

    for (frame = 0; frame < 20000; frame++) {
        seed = seed * 1664525u + 1013904223u;
        clock_ms += (seed >> 16) % 121;      /* 0..120 ms: up to ~4 tics */
        TryRunTics();
    }

    /* It has to have actually played: a loop that returned early every
     * frame would satisfy every "nothing went wrong" check below. */
    CU_ASSERT_TRUE(gametic - start > 20000);

    CU_ASSERT_PTR_NULL(players[0].message);
    assert_single_player_intact();
}

/*
 * The hook D_InitNetGame registered. I_Quit and I_Error both walk the
 * atexit list, so this runs on every exit from the game, clean or not --
 * and with FEATURE_MULTIPLAYER off it must have nothing to tear down.
 */
static void test_quit_hook_has_nothing_to_disconnect(void)
{
    CU_ASSERT_PTR_NOT_NULL(atexit_func);
    if (atexit_func != NULL) {
        atexit_func();
        atexit_func();
    }
    CU_ASSERT_EQUAL(n_errors, 0);
}

/* Nothing above may have talked the stub into believing in a network. */
static void test_network_stayed_absent_throughout(void)
{
    CU_ASSERT_FALSE(net_client_connected);
    CU_ASSERT_FALSE(drone);
    CU_ASSERT_EQUAL(n_advance_demo, 0);
    CU_ASSERT_PTR_NULL(last_error);
    assert_single_player_intact();
}

void suite_doom_net_tests(CU_pSuite s)
{
    CU_add_test(s, "stub describes an absent network",
                test_stub_describes_an_absent_network);
    CU_add_test(s, "connect finds no netgame",
                test_connect_finds_no_netgame);
    CU_add_test(s, "connect data carries no stack garbage",
                test_connect_data_carries_no_stack_garbage);
    CU_add_test(s, "start netgame invents one local player",
                test_start_netgame_invents_one_local_player);
    CU_add_test(s, "check netgame settles the player table",
                test_check_netgame_settles_the_player_table);
    CU_add_test(s, "first frame before the loop clock starts",
                test_first_frame_before_the_loop_clock_starts);
    CU_add_test(s, "one tic of time runs one tic of game",
                test_one_tic_of_time_runs_one_tic_of_game);
    CU_add_test(s, "stall drops time rather than inventing tics",
                test_stall_drops_time_rather_than_inventing_tics);
    CU_add_test(s, "phantom player is retired, not simulated",
                test_phantom_player_is_retired_not_simulated);
    CU_add_test(s, "demo playback leaves demo players alone",
                test_demo_playback_leaves_demo_players_alone);
    CU_add_test(s, "long session at an uneven frame rate",
                test_long_session_at_an_uneven_frame_rate);
    CU_add_test(s, "quit hook has nothing to disconnect",
                test_quit_hook_has_nothing_to_disconnect);
    CU_add_test(s, "network stayed absent throughout",
                test_network_stayed_absent_throughout);
}
