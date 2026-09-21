/*
 * tetris_logic.c -- see tetris_logic.h's header comment for why this file
 * lives directly under src/ and is dual-compiled (kernel + ring-3 blob).
 */

#include "tetris_logic.h"

/*
 * tetris_piece_rotations[shape][rotation]: a 4x4 bit-per-cell mask (bit
 * index = row*4+col, row 0 at the top of the box), one entry per shape per
 * 90-degree clockwise rotation from that shape's spawn orientation. Fixed
 * at compile time -- no rotation is ever computed at runtime, matching the
 * ticket's "fixed lookup table (4 rotations x 7 tetromino shapes)"
 * guidance instead of a general rotate-the-bitmask routine.
 */
const uint16_t tetris_piece_rotations[TETRIS_NUM_SHAPES][TETRIS_NUM_ROTATIONS] = {
    [TETRIS_SHAPE_I] = { 0x00F0, 0x4444, 0x0F00, 0x2222 },
    [TETRIS_SHAPE_O] = { 0x0660, 0x0660, 0x0660, 0x0660 },
    [TETRIS_SHAPE_T] = { 0x0270, 0x0464, 0x0E40, 0x2620 },
    [TETRIS_SHAPE_S] = { 0x0360, 0x0462, 0x06C0, 0x4620 },
    [TETRIS_SHAPE_Z] = { 0x0630, 0x0264, 0x0C60, 0x2640 },
    [TETRIS_SHAPE_J] = { 0x0470, 0x0644, 0x0E20, 0x2260 },
    [TETRIS_SHAPE_L] = { 0x0170, 0x0446, 0x0E80, 0x6220 },
};

int tetris_cell_filled(const tetris_game_t *g, int row, int col)
{
    if (row < 0 || row >= (int)TETRIS_ROWS || col < 0 || col >= (int)TETRIS_COLS)
        return 0;
    return g->board[row][col] != 0;
}

static uint32_t tetris_rng_next(tetris_game_t *g)
{
    uint32_t x = g->rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g->rng_state = x;
    return x;
}

/* Spawns g->next_shape as the new active piece, top-center of the board,
 * rotation 0, and rolls a fresh next_shape. If the freshly spawned piece
 * immediately collides (board filled to the brim), sets g->game_over
 * instead of leaving a piece embedded in the board -- this is the only
 * place spawn failure is detected, since the spawn box always starts at
 * row 0 and every other move only ever increases row (gravity/soft/hard
 * drop) or holds it fixed (rotate/left/right), so a piece already on the
 * board can never itself trigger a fresh spawn-collision check. */
static void spawn_piece(tetris_game_t *g)
{
    g->active.shape = g->next_shape;
    g->active.rotation = 0;
    g->active.col = (int8_t)((TETRIS_COLS - TETRIS_PIECE_DIM) / 2);
    g->active.row = 0;
    g->next_shape = (tetris_shape_t)(tetris_rng_next(g) % TETRIS_NUM_SHAPES);

    if (tetris_piece_collides(g, &g->active))
        g->game_over = 1;
}

void tetris_game_init(tetris_game_t *g, uint32_t seed)
{
    for (uint32_t r = 0; r < TETRIS_ROWS; r++)
        for (uint32_t c = 0; c < TETRIS_COLS; c++)
            g->board[r][c] = 0;

    g->score = 0;
    g->lines_cleared = 0;
    g->level = 0;
    g->game_over = 0;
    g->rng_state = seed ? seed : 1u; /* xorshift32 can't start at 0 */

    g->next_shape = (tetris_shape_t)(tetris_rng_next(g) % TETRIS_NUM_SHAPES);
    spawn_piece(g);
}

int tetris_piece_collides(const tetris_game_t *g, const tetris_piece_t *p)
{
    uint16_t mask = tetris_piece_rotations[p->shape][p->rotation];

    for (int lr = 0; lr < (int)TETRIS_PIECE_DIM; lr++) {
        for (int lc = 0; lc < (int)TETRIS_PIECE_DIM; lc++) {
            if (!(mask & (1u << (lr * (int)TETRIS_PIECE_DIM + lc))))
                continue;

            int r = p->row + lr;
            int c = p->col + lc;

            if (c < 0 || c >= (int)TETRIS_COLS)
                return 1;
            if (r >= (int)TETRIS_ROWS)
                return 1;
            if (r < 0)
                continue; /* above the visible board -- never actually
                           * reached since spawn starts at row 0 and row
                           * only ever increases, but harmless to allow. */
            if (g->board[r][c] != 0)
                return 1;
        }
    }
    return 0;
}

int tetris_try_move(tetris_game_t *g, int8_t dcol, int8_t drow)
{
    if (g->game_over)
        return 0;

    tetris_piece_t moved = g->active;
    moved.col = (int8_t)(moved.col + dcol);
    moved.row = (int8_t)(moved.row + drow);

    if (tetris_piece_collides(g, &moved))
        return 0;

    g->active = moved;
    return 1;
}

int tetris_try_rotate(tetris_game_t *g)
{
    if (g->game_over)
        return 0;

    tetris_piece_t rotated = g->active;
    rotated.rotation = (uint8_t)((rotated.rotation + 1) % TETRIS_NUM_ROTATIONS);

    if (tetris_piece_collides(g, &rotated))
        return 0;

    g->active = rotated;
    return 1;
}

uint32_t tetris_clear_lines(tetris_game_t *g)
{
    uint32_t cleared = 0;
    int write_row = (int)TETRIS_ROWS - 1;

    for (int read_row = (int)TETRIS_ROWS - 1; read_row >= 0; read_row--) {
        int full = 1;
        for (uint32_t c = 0; c < TETRIS_COLS; c++) {
            if (g->board[read_row][c] == 0) {
                full = 0;
                break;
            }
        }

        if (full) {
            cleared++;
            continue;
        }

        if (write_row != read_row) {
            for (uint32_t c = 0; c < TETRIS_COLS; c++)
                g->board[write_row][c] = g->board[read_row][c];
        }
        write_row--;
    }

    for (int r = write_row; r >= 0; r--) {
        for (uint32_t c = 0; c < TETRIS_COLS; c++)
            g->board[r][c] = 0;
    }

    return cleared;
}

/* Classic scoring: more simultaneous lines score super-linearly, scaled by
 * level (0-indexed, so level 0 is the base rate). */
static uint32_t score_for_clear(uint32_t lines, uint32_t level)
{
    static const uint32_t base[5] = { 0, 40, 100, 300, 1200 };
    if (lines > 4)
        lines = 4;
    return base[lines] * (level + 1);
}

void tetris_lock_active(tetris_game_t *g)
{
    uint16_t mask = tetris_piece_rotations[g->active.shape][g->active.rotation];
    uint8_t color = (uint8_t)(g->active.shape + 1);

    for (int lr = 0; lr < (int)TETRIS_PIECE_DIM; lr++) {
        for (int lc = 0; lc < (int)TETRIS_PIECE_DIM; lc++) {
            if (!(mask & (1u << (lr * (int)TETRIS_PIECE_DIM + lc))))
                continue;
            int r = g->active.row + lr;
            int c = g->active.col + lc;
            if (r >= 0 && r < (int)TETRIS_ROWS && c >= 0 && c < (int)TETRIS_COLS)
                g->board[r][c] = color;
        }
    }

    uint32_t cleared = tetris_clear_lines(g);
    if (cleared > 0) {
        g->score += score_for_clear(cleared, g->level);
        g->lines_cleared += cleared;
        g->level = g->lines_cleared / 10;
    }

    spawn_piece(g);
}

void tetris_gravity_tick(tetris_game_t *g)
{
    if (g->game_over)
        return;

    if (!tetris_try_move(g, 0, 1))
        tetris_lock_active(g);
}

uint32_t tetris_level_interval_ms(uint32_t level)
{
    uint32_t drop = level * 50u;
    if (drop >= 700u)
        return 100u;
    return 800u - drop;
}
