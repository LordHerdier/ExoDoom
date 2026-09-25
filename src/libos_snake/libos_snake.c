/*
 * libos_snake.c — Snake, running as a real ring-3 LibOS (SCRUM-182).
 *
 * Modeled directly on src/libos_wad_viewer/libos_wad_viewer.c: talks to the
 * kernel only through the ordinary exo_syscall.h ABI (exo_fb_acquire +
 * exo_page_map via src/libos_fb.c's libos_fb_map() for the screen,
 * exo_kbd_poll for input, exo_get_ticks for timing, exo_serial_write for
 * diagnostics), launched from the shell's "snake" command
 * (src/shell/shell_main.c) via exo_launch(EXO_LAUNCH_APP_SNAKE) (#21, src/syscall_launch.c)
 * rather than kernel_main hardcoding it as the boot-time LibOS.
 *
 * Simpler than the WAD viewer in two ways: there is no external resource to
 * stage before launch (no WAD mapping step), and no launch-time parameters
 * at all, so src/syscall_launch.c's sys_launch_snake() has no
 * libos_launch_patch_params() call and this file has no params struct.
 *
 * Like the WAD viewer's automap loop, this LibOS's main loop never returns
 * on its own -- it exits only via EXO_SYS_EXIT (#20, src/syscall_exit.c),
 * called directly through exo_syscall1() rather than the exo_exit()
 * convenience wrapper in exo_syscall.h. That wrapper's own body ends in a
 * `for (;;) {}`, which is enough for GCC to prove everything after a call to
 * it is unreachable -- true from the C abstract machine's point of view, but
 * not of the actual machine, since exo_exit()'s handler cannot
 * context_destroy() the calling context's own row (context_switch_request()
 * inside it still needs that row live, as the outgoing side, to capture
 * into) -- see libos_wad_viewer.c's own header comment for the full
 * reasoning. snake_exit_or_yield() below is the same raw-call-plus-fallback-
 * loop pattern that file's wad_viewer_exit_or_yield() uses.
 *
 * No libc shim is linked into this target (same as the WAD viewer) -- no
 * malloc, no rand(). The snake body lives in a fixed-size static array
 * (bounded by GRID_COLS*GRID_ROWS, independent of the actual framebuffer
 * resolution) and food placement uses a tiny hand-rolled xorshift32 PRNG
 * seeded from exo_get_ticks() at startup.
 *
 * Built by docker/scripts/build.sh's unconditional "[2e/7]" step, right
 * after the WAD viewer's "[2d/7]" -- see that step's own comment for why
 * these targets are built unconditionally and ahead of step 3's general
 * src/ *.c compile loop (this file must never be compiled with -DEXO_KERNEL,
 * which is why src/libos_snake/ is its own subdirectory rather than living
 * directly under src/).
 *
 * libos_snake_main() carries __attribute__((section(".text.entry"))) so it
 * lands at offset 0 of the linked .text blob regardless of source order or
 * how GCC schedules everything else -- see
 * tests/kernel/ring3_link_target.ld.in's own comment on why that attribute
 * exists. This file must still stay first in build.sh's source list for
 * this target, matching every other ring-3 target's convention.
 */

#include "exo_syscall.h"
#include "libos_fb.h"
#include "fb.h"
#include "fb_console.h"
#include "ps2.h"   /* ps2_key_t (KEY_UP/_DOWN/_LEFT/_RIGHT/_Q/_ESC) only --
                    * no kernel-only symbol from this header is called or
                    * linked in here. */

#include <stddef.h>
#include <stdint.h>

__attribute__((section(".text.entry")))
void libos_snake_main(void);

/* .data, not .rodata, and non-zero-initialized so it actually lands in
 * .data and not the zero-initialized .bss a plain `= 0` global would (GCC
 * never stores real bytes for a zero initializer) -- logged over
 * exo_serial_write right after entry, the same trick
 * libos_wad_viewer_banner uses, to prove the compiled .data region genuinely
 * loads at LIBOS_LAUNCH_DATA_VADDR. */
char libos_snake_banner[] = "ring3 snake: entering libos_snake_main\n";

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

/* No hlt/sleep syscall exists at ring 3 -- a busy poll on the monotonic tick
 * count is the only timing primitive available, same as
 * libos_wad_viewer.c's ring3_delay_ms(). */
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
 * libos_wad_viewer.c's wad_viewer_exit_or_yield() for why this must be the
 * raw exo_syscall1(EXO_SYS_EXIT, ...) + a real `for(;;) { exo_yield(); }`
 * rather than the exo_exit() convenience wrapper. */
static void snake_exit_or_yield(void)
{
    exo_syscall1(EXO_SYS_EXIT, 0);
    for (;;) { exo_yield(); }
}

/* ---- tiny PRNG (xorshift32) -- no rand() without the libc shim --------- */

static uint32_t rng_state;

static void rng_seed(uint32_t seed)
{
    rng_state = seed ? seed : 1u; /* xorshift32 can't start at 0 */
}

static uint32_t rng_next(void)
{
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

/* ---- game state ---------------------------------------------------------
 *
 * Fixed logical grid, independent of the actual framebuffer resolution --
 * cell size is derived from it instead, so the game looks right whether the
 * framebuffer is the QEMU default or something else. */

#define GRID_COLS 40u
#define GRID_ROWS 30u
#define MAX_SNAKE_LEN (GRID_COLS * GRID_ROWS)

typedef struct { uint8_t x, y; } cell_t;

typedef enum { DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT } dir_t;

static cell_t body[MAX_SNAKE_LEN];
static uint32_t body_len;
static dir_t direction;
static cell_t food;
static uint32_t score;

static int cell_in_body(uint8_t x, uint8_t y, uint32_t check_len)
{
    for (uint32_t i = 0; i < check_len; i++) {
        if (body[i].x == x && body[i].y == y)
            return 1;
    }
    return 0;
}

static void place_food(void)
{
    /* Bounded attempts: with MAX_SNAKE_LEN cells total and the snake never
     * exceeding that many, a handful of random tries almost always lands on
     * a free cell; falling back to a linear scan guarantees termination even
     * when the board is nearly full. */
    for (int attempt = 0; attempt < 64; attempt++) {
        uint8_t x = (uint8_t)(rng_next() % GRID_COLS);
        uint8_t y = (uint8_t)(rng_next() % GRID_ROWS);
        if (!cell_in_body(x, y, body_len)) {
            food.x = x;
            food.y = y;
            return;
        }
    }
    for (uint8_t y = 0; y < GRID_ROWS; y++) {
        for (uint8_t x = 0; x < GRID_COLS; x++) {
            if (!cell_in_body(x, y, body_len)) {
                food.x = x;
                food.y = y;
                return;
            }
        }
    }
    /* Board entirely full (a win, effectively) -- leave food wherever it
     * last was; the next move is guaranteed to be a collision. */
}

static void snake_reset(void)
{
    body_len = 3;
    body[0] = (cell_t){ GRID_COLS / 2,     GRID_ROWS / 2 };
    body[1] = (cell_t){ GRID_COLS / 2 - 1, GRID_ROWS / 2 };
    body[2] = (cell_t){ GRID_COLS / 2 - 2, GRID_ROWS / 2 };
    direction = DIR_RIGHT;
    score = 0;
    place_food();
}

/* Shifts every segment one slot towards the tail and writes the new head at
 * index 0. Iterating from the tail end up (rather than head-down) is what
 * makes this a plain shift instead of overwriting values before they're
 * copied. When grow is set, body_len is incremented first so the shift
 * covers one more slot and the old tail cell is retained (duplicated into
 * the new slot) rather than dropped. */
static void snake_advance(cell_t new_head, int grow)
{
    if (grow && body_len < MAX_SNAKE_LEN)
        body_len++;
    for (uint32_t i = body_len - 1; i > 0; i--)
        body[i] = body[i - 1];
    body[0] = new_head;
}

/* ---- rendering ----------------------------------------------------------
 *
 * Board area starts below a one-line HUD; cell size derived from whatever
 * the framebuffer actually reports. Full redraw of the board each move
 * keeps this simple and correct (no dirty-rect tracking) -- at a ~150ms
 * tick interval and at most GRID_COLS*GRID_ROWS cells, the cost is
 * negligible. */

#define HUD_HEIGHT_PX 16u /* exactly one fb_console_t text row (8x16 cells) */

typedef struct {
    uint32_t cell_w, cell_h;
    uint32_t origin_y;
} board_geom_t;

static void compute_board_geom(const framebuffer_t *fb, board_geom_t *g)
{
    g->origin_y = HUD_HEIGHT_PX;
    uint32_t usable_h = (fb->height > HUD_HEIGHT_PX)
                            ? (fb->height - HUD_HEIGHT_PX) : fb->height;
    g->cell_w = fb->width  / GRID_COLS;
    g->cell_h = usable_h   / GRID_ROWS;
    if (g->cell_w == 0) g->cell_w = 1;
    if (g->cell_h == 0) g->cell_h = 1;
}

static void draw_cell(framebuffer_t *fb, const board_geom_t *g,
                      uint8_t gx, uint8_t gy, uint8_t r, uint8_t gg, uint8_t b)
{
    fb_fill_rect(fb, gx * g->cell_w, g->origin_y + gy * g->cell_h,
                 g->cell_w, g->cell_h, r, gg, b);
}

/* Redrawn every tick, so the score digit count changing (5 -> 10) must not
 * leave stale glyphs behind from a longer previous line -- clear the whole
 * HUD strip and reset the cursor to (0,0) rather than relying on
 * fbcon_write to overwrite in place. Deliberately does not use
 * fbcon_clear(): that clears the *entire* framebuffer (src/fb_console.c),
 * which would erase the board too. Never emits '\n' -- one real newline
 * here would scroll the cursor onto the board area on the very next
 * render_hud() call, since fb_console_t has no independent notion of "HUD
 * row" vs "board row". */
static void render_hud(fb_console_t *con, framebuffer_t *fb, uint32_t cur_score)
{
    fb_fill_rect(fb, 0, 0, fb->width, HUD_HEIGHT_PX, 0, 0, 0);
    con->cursor_x = 0;
    con->cursor_y = 0;

    fbcon_set_color(con, 100, 220, 255, 0, 0, 0);
    fbcon_write(con, "SNAKE");
    fbcon_set_color(con, 60, 60, 60, 0, 0, 0);
    fbcon_write(con, " | score: ");
    fbcon_set_color(con, 220, 220, 220, 0, 0, 0);
    write_u32(con, cur_score);
    fbcon_set_color(con, 60, 60, 60, 0, 0, 0);
    fbcon_write(con, " | arrows to move, Q/Esc to quit");
}

static void render_board(framebuffer_t *fb, const board_geom_t *g)
{
    fb_fill_rect(fb, 0, g->origin_y, fb->width, GRID_ROWS * g->cell_h,
                0, 0, 0);
    for (uint32_t i = 0; i < body_len; i++) {
        uint8_t r = (i == 0) ? 100 : 40;
        uint8_t gg = (i == 0) ? 255 : 200;
        draw_cell(fb, g, body[i].x, body[i].y, r, gg, 60);
    }
    draw_cell(fb, g, food.x, food.y, 230, 60, 60);
}

static void render_game_over(fb_console_t *con, framebuffer_t *fb,
                             const board_geom_t *g, uint32_t final_score)
{
    fb_fill_rect(fb, 0, g->origin_y, fb->width, GRID_ROWS * g->cell_h,
                0, 0, 0);
    fbcon_set_color(con, 230, 50, 50, 0, 0, 0);
    fbcon_write(con, "\nGAME OVER");
    fbcon_set_color(con, 220, 220, 220, 0, 0, 0);
    fbcon_write(con, "  final score: ");
    write_u32(con, final_score);
    fbcon_write(con, "\npress any key to return to the shell\n");
}

/* ---- input --------------------------------------------------------------
 *
 * Buffers the latest direction key each poll and rejects a direct reversal
 * (down when already moving up, etc.) -- reversing into your own neck is an
 * instant, uninteresting death, not a real player choice. Returns 1 if Q/Esc
 * was pressed (caller should quit). */
static int poll_input(void)
{
    exo_kbd_event_t ev;
    while (exo_kbd_poll(&ev) > 0) {
        if (!ev.pressed)
            continue;
        switch (ev.key) {
        case KEY_UP:    if (direction != DIR_DOWN)  direction = DIR_UP;    break;
        case KEY_DOWN:  if (direction != DIR_UP)    direction = DIR_DOWN;  break;
        case KEY_LEFT:  if (direction != DIR_RIGHT) direction = DIR_LEFT;  break;
        case KEY_RIGHT: if (direction != DIR_LEFT)  direction = DIR_RIGHT; break;
        case KEY_Q:
        case KEY_ESC:
            return 1;
        default:
            break;
        }
    }
    return 0;
}

/* Drains and discards keyboard events until one press is seen, so the game-
 * over screen doesn't exit on a stale queued event from mid-game. */
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

#define TICK_INTERVAL_MS 150

/* ---- entry point --------------------------------------------------------- */

__attribute__((section(".text.entry")))
void libos_snake_main(void)
{
    ring3_log(libos_snake_banner);

    libos_fb_t libfb;
    if (libos_fb_map(&libfb) != 0 || libfb.vaddr == NULL) {
        ring3_log("ring3 snake: framebuffer map failed\n");
        snake_exit_or_yield();
    }

    framebuffer_t fb;
    fb_console_t con;
    if (!fb_init_bgrx8888(&fb, (uintptr_t)libfb.vaddr, libfb.pitch,
                          libfb.width, libfb.height, libfb.bpp)) {
        ring3_log("ring3 snake: fb_init_bgrx8888 failed\n");
        snake_exit_or_yield();
    }

    fb_clear(&fb, 0, 0, 0);
    fbcon_init(&con, &fb);

    board_geom_t geom;
    compute_board_geom(&fb, &geom);

    int64_t seed = ticks_now();
    rng_seed((uint32_t)seed ^ 0x9e3779b9u);

    snake_reset();

    ring3_log("ring3 snake: entering game loop\n");

    int64_t last_move = ticks_now();
    render_hud(&con, &fb, score);
    render_board(&fb, &geom);

    for (;;) {
        if (poll_input()) {
            ring3_log("ring3 snake: quit, handing off to the shell\n");
            snake_exit_or_yield();
        }

        int64_t now = ticks_now();
        if (now - last_move < TICK_INTERVAL_MS)
            continue;
        last_move = now;

        cell_t head = body[0];
        cell_t new_head = head;
        switch (direction) {
        case DIR_UP:    new_head.y--; break;
        case DIR_DOWN:  new_head.y++; break;
        case DIR_LEFT:  new_head.x--; break;
        case DIR_RIGHT: new_head.x++; break;
        }

        int hit_wall = (direction == DIR_UP    && head.y == 0) ||
                      (direction == DIR_DOWN  && head.y == GRID_ROWS - 1) ||
                      (direction == DIR_LEFT  && head.x == 0) ||
                      (direction == DIR_RIGHT && head.x == GRID_COLS - 1);

        int grow = !hit_wall && new_head.x == food.x && new_head.y == food.y;
        uint32_t check_len = grow ? body_len : body_len - 1;
        int hit_self = !hit_wall && cell_in_body(new_head.x, new_head.y, check_len);

        if (hit_wall || hit_self) {
            ring3_log("ring3 snake: collision, game over\n");
            render_game_over(&con, &fb, &geom, score);
            wait_for_keypress();
            snake_exit_or_yield();
        }

        snake_advance(new_head, grow);
        if (grow) {
            score++;
            place_food();
        }

        render_board(&fb, &geom);
        render_hud(&con, &fb, score);

        /* Ring 3 cannot execute `hlt` -- a tight poll loop is the only
         * option when nothing else needs the CPU between ticks. */
    }
}
