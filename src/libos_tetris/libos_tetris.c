/*
 * libos_tetris.c — Tetris, running as a real ring-3 LibOS (SCRUM-183).
 *
 * Modeled directly on src/libos_snake/libos_snake.c: talks to the kernel
 * only through the ordinary exo_syscall.h ABI (exo_fb_acquire + exo_page_map
 * via src/libos_fb.c's libos_fb_map() for the screen, exo_kbd_poll for
 * input, exo_get_ticks for gravity timing, exo_serial_write for
 * diagnostics), launched from the shell's "tetris" command
 * (src/shell/shell_main.c) via exo_launch_tetris() (src/syscall_launch.c).
 *
 * Same two simplifications as Snake relative to the WAD viewer: no external
 * resource to stage before launch, and no launch-time parameters, so
 * src/syscall_launch.c's sys_launch_tetris() has no
 * libos_launch_patch_params() call and this file has no params struct.
 *
 * All actual game state and rules (the board, piece rotation table,
 * collision/lock/line-clear/scoring) live in src/tetris_logic.c/.h instead
 * of here -- that file is ordinary kernel code (no exo_* call, no
 * EXO_KERNEL ifdef), compiled twice: once into the kernel binary via step
 * 3's plain src/ *.c glob, where tests/kernel/test_libos_tetris_k.c can
 * unit-test the line-clear logic directly from ring 0, and once here, into
 * this ring-3 blob, via this target's own build_ring3_link_target source
 * list in docker/scripts/build.sh. This file is only the ring-3-only
 * shell: framebuffer setup, rendering, input polling, and the gravity/exit
 * loop around tetris_logic's pure state machine.
 *
 * Like Snake's main loop, this LibOS never returns on its own -- it exits
 * only via EXO_SYS_EXIT (#20, src/syscall_exit.c), called directly through
 * exo_syscall1() rather than the exo_exit() convenience wrapper, for the
 * same reason libos_snake.c's own header comment gives (exo_exit()'s body
 * ends in a `for (;;) {}` GCC can prove unreachable, but the real machine
 * still needs to fall through to a live for(;;) { exo_yield(); } once
 * rescheduled back onto this context after the switch it triggers).
 * tetris_exit_or_yield() below is that same pattern.
 *
 * No libc shim is linked into this target (same as Snake) -- no malloc,
 * no rand(); tetris_logic.c's own tiny xorshift32 PRNG (seeded from
 * exo_get_ticks() here at startup) is what picks tetromino shapes.
 *
 * libos_tetris_main() carries __attribute__((section(".text.entry"))) so
 * it lands at offset 0 of the linked .text blob regardless of source order
 * -- see tests/kernel/ring3_link_target.ld.in's own comment. This file
 * must still stay first in build.sh's source list for this target, matching
 * every other ring-3 target's convention.
 */

#include "exo_syscall.h"
#include "libos_fb.h"
#include "fb.h"
#include "fb_console.h"
#include "tetris_logic.h"
#include "ps2.h"   /* ps2_key_t (KEY_UP/_DOWN/_LEFT/_RIGHT/_SPACE/_Q/_ESC)
                    * only -- no kernel-only symbol from this header is
                    * called or linked in here. */

#include <stddef.h>
#include <stdint.h>

__attribute__((section(".text.entry")))
void libos_tetris_main(void);

/* .data, not .rodata, and non-zero-initialized so it actually lands in
 * .data and not the zero-initialized .bss a plain `= 0` global would (GCC
 * never stores real bytes for a zero initializer) -- logged over
 * exo_serial_write right after entry, the same trick
 * libos_snake_banner/libos_wad_viewer_banner use, to prove the compiled
 * .data region genuinely loads at LIBOS_LAUNCH_DATA_VADDR. */
char libos_tetris_banner[] = "ring3 tetris: entering libos_tetris_main\n";

/* ---- small ring-3-only helpers (no libc shim linked into this target) -- */

static uint32_t ring3_strlen(const char *s)
{
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

static void ring3_log(const char *s)
{
    exo_serial_write(s, ring3_strlen(s));
}

/* No hlt/sleep syscall exists at ring 3 -- a busy poll on the monotonic
 * tick count is the only timing primitive available, same as
 * libos_snake.c's ticks_now(). */
static int64_t ticks_now(void)
{
    return exo_get_ticks();
}

static void write_u32(fb_console_t *con, uint32_t val)
{
    if (val == 0) { fbcon_write(con, "0"); return; }
    char buf[11];
    buf[10] = '\0';
    int i = 10;
    while (val > 0) { buf[--i] = '0' + (val % 10); val /= 10; }
    fbcon_write(con, &buf[i]);
}

/* Every quit/game-over path wants "exit, or yield forever if resumed
 * anyway" -- see this file's header comment and
 * libos_snake.c's snake_exit_or_yield() for why this must be the raw
 * exo_syscall1(EXO_SYS_EXIT, ...) + a real `for(;;) { exo_yield(); }`
 * rather than the exo_exit() convenience wrapper. */
static void tetris_exit_or_yield(void)
{
    exo_syscall1(EXO_SYS_EXIT, 0);
    for (;;) { exo_yield(); }
}

/* ---- rendering ------------------------------------------------------------
 *
 * Board area starts below a one-line HUD; a small "next piece" preview box
 * sits to the right of the well. Cell size is derived from whatever the
 * framebuffer actually reports, same approach as libos_snake.c's
 * compute_board_geom(). Full redraw of the board every tick keeps this
 * simple and correct (no dirty-rect tracking) -- at the gravity tick
 * interval and TETRIS_COLS*TETRIS_ROWS cells, the cost is negligible.
 */

#define HUD_HEIGHT_PX 16u /* exactly one fb_console_t text row (8x16 cells) */
#define PREVIEW_COLS 6u   /* well + a little breathing room + 4-wide preview */

typedef struct {
    uint32_t cell_px;
    uint32_t origin_x, origin_y;
    uint32_t preview_origin_x, preview_origin_y;
} board_geom_t;

static void compute_board_geom(const framebuffer_t *fb, board_geom_t *g)
{
    g->origin_y = HUD_HEIGHT_PX;
    uint32_t usable_h = (fb->height > HUD_HEIGHT_PX)
                            ? (fb->height - HUD_HEIGHT_PX) : fb->height;

    uint32_t cell_w = fb->width / (TETRIS_COLS + PREVIEW_COLS);
    uint32_t cell_h = usable_h / TETRIS_ROWS;
    g->cell_px = (cell_w < cell_h) ? cell_w : cell_h;
    if (g->cell_px == 0) g->cell_px = 1;

    g->origin_x = 0;
    g->preview_origin_x = TETRIS_COLS * g->cell_px + g->cell_px;
    g->preview_origin_y = g->origin_y + g->cell_px;
}

/* Color palette indexed by tetris_shape_t + 1 (0 is "empty", never drawn) --
 * kept in this file rather than tetris_logic.c since color is a rendering
 * concern, not game-state. */
static const struct { uint8_t r, g, b; } shape_colors[TETRIS_NUM_SHAPES] = {
    [TETRIS_SHAPE_I] = { 80, 220, 220 },
    [TETRIS_SHAPE_O] = { 220, 220, 80 },
    [TETRIS_SHAPE_T] = { 180, 80, 220 },
    [TETRIS_SHAPE_S] = { 90, 220, 90 },
    [TETRIS_SHAPE_Z] = { 220, 90, 90 },
    [TETRIS_SHAPE_J] = { 90, 110, 220 },
    [TETRIS_SHAPE_L] = { 220, 150, 60 },
};

static void draw_cell(framebuffer_t *fb, const board_geom_t *g,
                      uint32_t px, uint32_t py, uint8_t r, uint8_t gg, uint8_t b)
{
    fb_fill_rect(fb, px, py, g->cell_px, g->cell_px, r, gg, b);
}

static void render_hud(fb_console_t *con, framebuffer_t *fb,
                       const tetris_game_t *game)
{
    fb_fill_rect(fb, 0, 0, fb->width, HUD_HEIGHT_PX, 0, 0, 0);
    con->cursor_x = 0;
    con->cursor_y = 0;

    fbcon_set_color(con, 100, 220, 255, 0, 0, 0);
    fbcon_write(con, "TETRIS");
    fbcon_set_color(con, 60, 60, 60, 0, 0, 0);
    fbcon_write(con, " | score: ");
    fbcon_set_color(con, 220, 220, 220, 0, 0, 0);
    write_u32(con, game->score);
    fbcon_set_color(con, 60, 60, 60, 0, 0, 0);
    fbcon_write(con, " | lines: ");
    fbcon_set_color(con, 220, 220, 220, 0, 0, 0);
    write_u32(con, game->lines_cleared);
    fbcon_set_color(con, 60, 60, 60, 0, 0, 0);
    fbcon_write(con, " | level: ");
    fbcon_set_color(con, 220, 220, 220, 0, 0, 0);
    write_u32(con, game->level);
    fbcon_set_color(con, 60, 60, 60, 0, 0, 0);
    fbcon_write(con, " | Q/Esc to quit");
}

static void render_board(framebuffer_t *fb, const board_geom_t *g,
                         const tetris_game_t *game)
{
    fb_fill_rect(fb, g->origin_x, g->origin_y,
                TETRIS_COLS * g->cell_px, TETRIS_ROWS * g->cell_px, 0, 0, 0);

    for (uint32_t r = 0; r < TETRIS_ROWS; r++) {
        for (uint32_t c = 0; c < TETRIS_COLS; c++) {
            uint8_t cell = game->board[r][c];
            if (cell == 0)
                continue;
            const uint8_t sh = (uint8_t)(cell - 1);
            draw_cell(fb, g, g->origin_x + c * g->cell_px,
                     g->origin_y + r * g->cell_px,
                     shape_colors[sh].r, shape_colors[sh].g, shape_colors[sh].b);
        }
    }

    uint16_t mask = tetris_piece_rotations[game->active.shape][game->active.rotation];
    const uint8_t r = shape_colors[game->active.shape].r;
    const uint8_t gg = shape_colors[game->active.shape].g;
    const uint8_t b = shape_colors[game->active.shape].b;
    for (int lr = 0; lr < (int)TETRIS_PIECE_DIM; lr++) {
        for (int lc = 0; lc < (int)TETRIS_PIECE_DIM; lc++) {
            if (!(mask & (1u << (lr * (int)TETRIS_PIECE_DIM + lc))))
                continue;
            int row = game->active.row + lr;
            int col = game->active.col + lc;
            if (row < 0 || row >= (int)TETRIS_ROWS ||
               col < 0 || col >= (int)TETRIS_COLS)
                continue;
            draw_cell(fb, g, g->origin_x + (uint32_t)col * g->cell_px,
                     g->origin_y + (uint32_t)row * g->cell_px, r, gg, b);
        }
    }
}

static void render_preview(framebuffer_t *fb, const board_geom_t *g,
                           const tetris_game_t *game)
{
    fb_fill_rect(fb, g->preview_origin_x, g->preview_origin_y,
                TETRIS_PIECE_DIM * g->cell_px, TETRIS_PIECE_DIM * g->cell_px,
                0, 0, 0);

    uint16_t mask = tetris_piece_rotations[game->next_shape][0];
    const uint8_t r = shape_colors[game->next_shape].r;
    const uint8_t gg = shape_colors[game->next_shape].g;
    const uint8_t b = shape_colors[game->next_shape].b;
    for (int lr = 0; lr < (int)TETRIS_PIECE_DIM; lr++) {
        for (int lc = 0; lc < (int)TETRIS_PIECE_DIM; lc++) {
            if (!(mask & (1u << (lr * (int)TETRIS_PIECE_DIM + lc))))
                continue;
            draw_cell(fb, g, g->preview_origin_x + (uint32_t)lc * g->cell_px,
                     g->preview_origin_y + (uint32_t)lr * g->cell_px, r, gg, b);
        }
    }
}

static void render_game_over(fb_console_t *con, framebuffer_t *fb,
                             const board_geom_t *g, const tetris_game_t *game)
{
    fb_fill_rect(fb, g->origin_x, g->origin_y,
                TETRIS_COLS * g->cell_px, TETRIS_ROWS * g->cell_px, 0, 0, 0);
    fbcon_set_color(con, 230, 50, 50, 0, 0, 0);
    fbcon_write(con, "\nGAME OVER");
    fbcon_set_color(con, 220, 220, 220, 0, 0, 0);
    fbcon_write(con, "  final score: ");
    write_u32(con, game->score);
    fbcon_write(con, "\npress any key to return to the shell\n");
}

/* ---- input ----------------------------------------------------------------
 *
 * Left/right move, down = soft drop (one row per press, gravity still runs
 * independently), up = rotate, space = hard drop (repeat soft-drop moves
 * until it settles, then lock immediately rather than waiting for the next
 * gravity tick). Returns 1 if Q/Esc was pressed (caller should quit). */
static int poll_input(tetris_game_t *game)
{
    exo_kbd_event_t ev;
    while (exo_kbd_poll(&ev) > 0) {
        if (!ev.pressed)
            continue;
        switch (ev.key) {
        case KEY_LEFT:  tetris_try_move(game, -1, 0); break;
        case KEY_RIGHT: tetris_try_move(game, 1, 0);  break;
        case KEY_DOWN:  tetris_try_move(game, 0, 1);  break;
        case KEY_UP:    tetris_try_rotate(game);      break;
        case KEY_SPACE:
            while (tetris_try_move(game, 0, 1)) { }
            tetris_lock_active(game);
            break;
        case KEY_Q:
        case KEY_ESC:
            return 1;
        default:
            break;
        }
    }
    return 0;
}

/* Drains and discards keyboard events until one press is seen, so the
 * game-over screen doesn't exit on a stale queued event from mid-game --
 * same as libos_snake.c's wait_for_keypress(). */
static void wait_for_keypress(void)
{
    exo_kbd_event_t ev;
    for (;;) {
        while (exo_kbd_poll(&ev) > 0) {
            if (ev.pressed)
                return;
        }
    }
}

/* ---- entry point ----------------------------------------------------------- */

__attribute__((section(".text.entry")))
void libos_tetris_main(void)
{
    ring3_log(libos_tetris_banner);

    libos_fb_t libfb;
    if (libos_fb_map(&libfb) != 0 || libfb.vaddr == NULL) {
        ring3_log("ring3 tetris: framebuffer map failed\n");
        tetris_exit_or_yield();
    }

    framebuffer_t fb;
    fb_console_t con;
    if (!fb_init_bgrx8888(&fb, (uintptr_t)libfb.vaddr, libfb.pitch,
                          libfb.width, libfb.height, libfb.bpp)) {
        ring3_log("ring3 tetris: fb_init_bgrx8888 failed\n");
        tetris_exit_or_yield();
    }

    fb_clear(&fb, 0, 0, 0);
    fbcon_init(&con, &fb);

    board_geom_t geom;
    compute_board_geom(&fb, &geom);

    tetris_game_t game;
    tetris_game_init(&game, (uint32_t)ticks_now() ^ 0x9e3779b9u);

    ring3_log("ring3 tetris: entering game loop\n");

    int64_t last_drop = ticks_now();
    render_hud(&con, &fb, &game);
    render_board(&fb, &geom, &game);
    render_preview(&fb, &geom, &game);

    for (;;) {
        if (poll_input(&game)) {
            ring3_log("ring3 tetris: quit, handing off to the shell\n");
            tetris_exit_or_yield();
        }

        int64_t now = ticks_now();
        uint32_t interval = tetris_level_interval_ms(game.level);
        if (now - last_drop >= (int64_t)interval) {
            last_drop = now;
            tetris_gravity_tick(&game);
        }

        if (game.game_over) {
            ring3_log("ring3 tetris: top-out, game over\n");
            render_game_over(&con, &fb, &geom, &game);
            wait_for_keypress();
            tetris_exit_or_yield();
        }

        render_board(&fb, &geom, &game);
        render_preview(&fb, &geom, &game);
        render_hud(&con, &fb, &game);

        /* Ring 3 cannot execute `hlt` -- a tight poll loop is the only
         * option when nothing else needs the CPU between ticks. */
    }
}
