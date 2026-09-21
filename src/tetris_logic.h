/*
 * tetris_logic.h -- pure Tetris game-state logic (SCRUM-183), syscall-free.
 *
 * Deliberately lives directly under src/ rather than src/libos_tetris/ (see
 * src/libos_tetris/libos_tetris.c's own header comment): this file calls no
 * exo_* syscall and needs no EXO_KERNEL ifdef, so it is safe to compile
 * twice, exactly like src/fb.c/src/fb_console.c already are --
 *
 *   - once through docker/scripts/build.sh step 3's ordinary src/ *.c glob
 *     (-DEXO_KERNEL), linked into the kernel binary, where
 *     tests/kernel/test_libos_tetris_k.c can call it directly from ring 0;
 *   - once explicitly listed in the libos_tetris build_ring3_link_target
 *     call (no -DEXO_KERNEL), linked into the ring-3 Tetris blob.
 *
 * That is what makes the "line-clear logic" half of this ticket's KUnit
 * acceptance criterion possible at all -- src/libos_tetris/libos_tetris.c
 * itself is ring-3-only (no libc shim, no test harness reaches it), the same
 * way src/libos_snake/libos_snake.c's game logic isn't unit-testable from
 * tests/kernel/ today.
 *
 * All fixed/integer -- no double/float, consistent with the kernel's
 * -mno-sse build (see src/fixed_math.h and CLAUDE.md's SCRUM-41 note).
 * Piece rotation is a plain 4-orientations x 7-shapes lookup table, not a
 * computed rotation, so no trig is needed either.
 */

#ifndef TETRIS_LOGIC_H
#define TETRIS_LOGIC_H

#include <stdint.h>

#define TETRIS_COLS 10u
#define TETRIS_ROWS 20u

/* Cell size within a piece's 4x4 bounding box, matching the classic SRS
 * convention every tetromino rotation table is normally expressed in. */
#define TETRIS_PIECE_DIM 4u

#define TETRIS_NUM_SHAPES 7u
#define TETRIS_NUM_ROTATIONS 4u

/* Shape indices into tetris_piece_rotations -- also used as the grid's
 * filled-cell color index (1..7); 0 means "empty" throughout this file. */
typedef enum {
    TETRIS_SHAPE_I = 0,
    TETRIS_SHAPE_O,
    TETRIS_SHAPE_T,
    TETRIS_SHAPE_S,
    TETRIS_SHAPE_Z,
    TETRIS_SHAPE_J,
    TETRIS_SHAPE_L,
} tetris_shape_t;

/* tetris_piece_rotations[shape][rotation] is a 4x4 bitmask, one bit per
 * cell (bit (row*4+col)), row 0 at the top -- the fixed lookup table the
 * ticket calls for in place of computed rotation. Defined in
 * tetris_logic.c. */
extern const uint16_t tetris_piece_rotations[TETRIS_NUM_SHAPES][TETRIS_NUM_ROTATIONS];

typedef struct {
    tetris_shape_t shape;
    uint8_t rotation;   /* 0..TETRIS_NUM_ROTATIONS-1 */
    int8_t  col, row;   /* position of the piece's 4x4 box's top-left cell */
} tetris_piece_t;

typedef struct {
    /* board[row][col], 0 = empty, else a color index == shape+1 so a locked
     * cell keeps its tetromino's color without a separate lookup. */
    uint8_t board[TETRIS_ROWS][TETRIS_COLS];

    tetris_piece_t active;
    tetris_shape_t next_shape;

    uint32_t score;
    uint32_t lines_cleared;
    uint32_t level;

    /* Set once the active piece cannot be placed at spawn -- board is full
     * to the brim. The caller stops advancing gravity/accepting moves once
     * this is set. */
    int game_over;

    /* xorshift32 state for shape selection -- caller seeds it once at
     * startup (e.g. from exo_get_ticks()); tetris_logic.c never reads
     * exo_get_ticks() itself. */
    uint32_t rng_state;
} tetris_game_t;

/* Returns nonzero if board cell (row, col) is set. Bounds are the caller's
 * responsibility internally; exposed mainly for tests. */
int tetris_cell_filled(const tetris_game_t *g, int row, int col);

/* Seeds the RNG (0 is remapped to a nonzero value, same reasoning as every
 * other xorshift32 use in this tree) and resets the whole game to a fresh
 * board with a freshly spawned active piece and a rolled next_shape. */
void tetris_game_init(tetris_game_t *g, uint32_t seed);

/* True if `p` at its current (row, col, rotation) collides with the board
 * boundary or an occupied board cell. Used both to validate a proposed move
 * and, internally, to detect spawn failure (game over). */
int tetris_piece_collides(const tetris_game_t *g, const tetris_piece_t *p);

/* Movement/rotation attempts: each validates via tetris_piece_collides()
 * and only commits (mutates g->active) on success. Returns 1 if the move
 * was applied, 0 if it was rejected. No effect (returns 0) once
 * g->game_over is set. */
int tetris_try_move(tetris_game_t *g, int8_t dcol, int8_t drow);
int tetris_try_rotate(tetris_game_t *g);

/* Locks the active piece into the board, clears any completed rows
 * (compacting the board so rows above shift down -- see
 * tetris_clear_lines()), updates score/lines_cleared/level, and spawns the
 * next piece (rolling a new next_shape). If the freshly spawned piece
 * immediately collides, sets g->game_over instead of leaving a piece
 * embedded in the board. */
void tetris_lock_active(tetris_game_t *g);

/* One gravity step: try to move the active piece down; if that fails,
 * lock it (via tetris_lock_active()) instead. This is what the LibOS's
 * main loop calls on every gravity tick. */
void tetris_gravity_tick(tetris_game_t *g);

/* Scans the board for full rows, removes them, and shifts every row above
 * each cleared row down by one (so the top of the board fills with empty
 * rows) -- exposed directly for the line-clear KUnit tests, and also used
 * internally by tetris_lock_active(). Returns the number of rows cleared. */
uint32_t tetris_clear_lines(tetris_game_t *g);

/* Gravity tick interval in milliseconds for a given level -- speeds up
 * (shorter interval) as level increases, floored so it never reaches an
 * unplayable near-zero value. */
uint32_t tetris_level_interval_ms(uint32_t level);

#endif /* TETRIS_LOGIC_H */
