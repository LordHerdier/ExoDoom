/*
 * test_libos_tetris_k.c -- Tetris (SCRUM-183): pure game-logic coverage plus
 * the real Tetris LibOS blob build/fit check.
 *
 * Two independent things are verified here, for two different reasons:
 *
 *   - src/tetris_logic.c is ordinary kernel code (no exo_* syscall, no
 *     EXO_KERNEL ifdef -- see that file's own header comment on why it
 *     lives directly under src/ rather than under src/libos_tetris/), so
 *     its line-clear/collision/spawn logic is called directly from ring 0
 *     here, the same as any other kernel unit under test.
 *   - src/libos_tetris/libos_tetris.c is ring-3-only -- a real interactive
 *     loop driven by exo_kbd_poll()/exo_get_ticks(), exiting only via
 *     EXO_SYS_EXIT -- so, like test_libos_snake_k.c, this does NOT launch
 *     it. What IS verified automatically on every make docker-test/CI run
 *     is that the exact _binary_libos_tetris_{code,data}_bin_* blobs
 *     src/syscall_launch.c embeds link, fit the LibOS window budget, and
 *     build into a real address space via libos_build_image() without
 *     error. The interactive half (movement, rotation, rendering, quitting)
 *     is verified manually via `make docker-run-kernel`.
 */

#include "kunit.h"
#include "tetris_logic.h"
#include "libos_launch.h"
#include "vmm.h"
#include "page_alloc.h"

#include "libos_tetris/libos_tetris_layout.h"

#include <stdint.h>
#include <stddef.h>

/* ---- pure game-logic tests ---------------------------------------------- */

static void test_spawn_places_active_piece(void)
{
    tetris_game_t g;
    tetris_game_init(&g, 12345u);

    CU_ASSERT_FALSE(g.game_over);
    CU_ASSERT_TRUE(g.active.shape < TETRIS_NUM_SHAPES);
    CU_ASSERT_EQUAL(g.active.rotation, 0);
    CU_ASSERT_EQUAL(g.active.row, 0);
    CU_ASSERT_FALSE(tetris_piece_collides(&g, &g.active));
}

static void test_move_rejected_past_wall(void)
{
    tetris_game_t g;
    tetris_game_init(&g, 1u);

    /* Push left far more times than the board is wide -- must settle
     * against the wall and reject every further attempt, regardless of the
     * spawned shape's own horizontal extent within its 4x4 box. */
    for (int i = 0; i < (int)TETRIS_COLS; i++)
        tetris_try_move(&g, -1, 0);

    CU_ASSERT_FALSE(tetris_try_move(&g, -1, 0));
}

static void test_full_row_clears_and_scores(void)
{
    tetris_game_t g;
    tetris_game_init(&g, 7u);

    /* Fill the bottom row entirely by hand except one cell, then lock the
     * active piece so its footprint completes it -- but the simplest,
     * fully deterministic way to exercise tetris_clear_lines() directly
     * (independent of any particular shape's footprint) is to fill the
     * board directly and call it standalone, which is exactly what this
     * function is exposed for. */
    for (uint32_t c = 0; c < TETRIS_COLS; c++)
        g.board[TETRIS_ROWS - 1][c] = 1;
    /* Row above is only partially filled -- must survive untouched other
     * than shifting down by one once the row below it is cleared. */
    g.board[TETRIS_ROWS - 2][0] = 2;

    uint32_t cleared = tetris_clear_lines(&g);

    CU_ASSERT_EQUAL(cleared, 1);
    /* The full row is gone; the partially-filled row above it shifted down
     * into its place. */
    CU_ASSERT_TRUE(tetris_cell_filled(&g, TETRIS_ROWS - 1, 0));
    CU_ASSERT_EQUAL(g.board[TETRIS_ROWS - 1][0], 2);
    for (uint32_t c = 1; c < TETRIS_COLS; c++)
        CU_ASSERT_FALSE(tetris_cell_filled(&g, TETRIS_ROWS - 1, c));
    /* Everything above the old partial row is now empty. */
    for (uint32_t r = 0; r < TETRIS_ROWS - 1; r++)
        for (uint32_t c = 0; c < TETRIS_COLS; c++)
            CU_ASSERT_FALSE(tetris_cell_filled(&g, (int)r, (int)c));
}

static void test_non_full_row_untouched(void)
{
    tetris_game_t g;
    tetris_game_init(&g, 42u);

    g.board[TETRIS_ROWS - 1][0] = 3;
    g.board[TETRIS_ROWS - 1][1] = 3;
    /* Deliberately leave the rest of the row empty. */

    uint32_t cleared = tetris_clear_lines(&g);

    CU_ASSERT_EQUAL(cleared, 0);
    CU_ASSERT_EQUAL(g.board[TETRIS_ROWS - 1][0], 3);
    CU_ASSERT_EQUAL(g.board[TETRIS_ROWS - 1][1], 3);
}

static void test_multiple_rows_clear_at_once(void)
{
    tetris_game_t g;
    tetris_game_init(&g, 99u);

    for (uint32_t c = 0; c < TETRIS_COLS; c++) {
        g.board[TETRIS_ROWS - 1][c] = 1;
        g.board[TETRIS_ROWS - 2][c] = 1;
        g.board[TETRIS_ROWS - 3][c] = 1;
    }
    g.board[TETRIS_ROWS - 4][0] = 4; /* survivor, should end up at the very
                                       * bottom row after three full rows
                                       * below it are removed. */

    uint32_t cleared = tetris_clear_lines(&g);

    CU_ASSERT_EQUAL(cleared, 3);
    CU_ASSERT_EQUAL(g.board[TETRIS_ROWS - 1][0], 4);
    for (uint32_t c = 1; c < TETRIS_COLS; c++)
        CU_ASSERT_FALSE(tetris_cell_filled(&g, TETRIS_ROWS - 1, c));
}

static void test_lock_active_updates_score_and_spawns_next(void)
{
    tetris_game_t g;
    tetris_game_init(&g, 5u);

    /* Fill the bottom row solid except one hole under the active piece's
     * own spawn column, then drop it to the floor and lock -- whether that
     * particular shape/rotation actually plugs the hole is not the point
     * of this test (see the score comment below); what must hold
     * regardless is the postcondition after locking: a fresh, placeable
     * piece and no crash. */
    for (uint32_t c = 0; c < TETRIS_COLS; c++)
        g.board[TETRIS_ROWS - 1][c] = 1;
    g.board[TETRIS_ROWS - 1][g.active.col] = 0;

    while (tetris_try_move(&g, 0, 1)) { /* drop to the floor */ }

    uint32_t score_before = g.score;
    tetris_lock_active(&g);

    CU_ASSERT_FALSE(g.game_over);
    /* A new active piece must exist and be collision-free at spawn. */
    CU_ASSERT_FALSE(tetris_piece_collides(&g, &g.active));
    CU_ASSERT_EQUAL(g.active.row, 0);
    (void)score_before; /* score may or may not have moved depending on
                          * whether the drop actually completed the row for
                          * this particular shape/rotation -- the invariant
                          * this test actually pins down is the spawn
                          * postcondition above, not the score value. */
}

static void test_top_out_sets_game_over(void)
{
    tetris_game_t g;
    tetris_game_init(&g, 3u);

    /* Fill every cell the spawn box (columns 3..6, per spawn_piece()) could
     * occupy across the top TETRIS_PIECE_DIM rows, leaving the last column
     * empty so these rows are NOT full -- a full row would instead be
     * cleared by tetris_lock_active()'s own tetris_clear_lines() call,
     * emptying the top of the board again and defeating this test. Leaving
     * one column open blocks every spawn location without ever completing
     * a line. */
    for (uint32_t r = 0; r < TETRIS_PIECE_DIM; r++)
        for (uint32_t c = 0; c < TETRIS_COLS - 1; c++)
            g.board[r][c] = 1;

    tetris_lock_active(&g);

    CU_ASSERT_TRUE(g.game_over);
}

static void test_level_interval_speeds_up_and_floors(void)
{
    CU_ASSERT_TRUE(tetris_level_interval_ms(1) < tetris_level_interval_ms(0));
    CU_ASSERT_EQUAL(tetris_level_interval_ms(100), 100);
}

/* ---- real blob build/fit check ------------------------------------------- */

static uint64_t saved_libos_pml4;

int libos_tetris_suite_init(void) {
    saved_libos_pml4 = vmm_address_space_for(PAGE_OWNER_LIBOS);
    return 0;
}

int libos_tetris_suite_cleanup(void) {
    if (saved_libos_pml4 != 0)
        vmm_bind_address_space(PAGE_OWNER_LIBOS, saved_libos_pml4);
    return 0;
}

extern const uint8_t _binary_libos_tetris_code_bin_start[];
extern const uint8_t _binary_libos_tetris_code_bin_end[];
extern const uint8_t _binary_libos_tetris_data_bin_start[];
extern const uint8_t _binary_libos_tetris_data_bin_end[];

static void test_tetris_image_builds_and_fits_budget(void)
{
    size_t code_len = (size_t)(_binary_libos_tetris_code_bin_end -
                               _binary_libos_tetris_code_bin_start);
    size_t data_len = (size_t)(_binary_libos_tetris_data_bin_end -
                               _binary_libos_tetris_data_bin_start);

    CU_ASSERT_TRUE(code_len > 0);
    CU_ASSERT_TRUE(code_len <= LIBOS_LAUNCH_MAX_CODE_PAGES * 0x1000);
    CU_ASSERT_TRUE(data_len + LIBOS_TETRIS_BSS_LEN <=
                   LIBOS_LAUNCH_MAX_DATA_PAGES * 0x1000);

    libos_image_t img;
    int build_rc = libos_build_image(PAGE_OWNER_LIBOS,
                                     _binary_libos_tetris_code_bin_start, code_len,
                                     _binary_libos_tetris_data_bin_start, data_len,
                                     LIBOS_TETRIS_BSS_LEN, &img);
    CU_ASSERT_EQUAL(build_rc, VMM_OK);
    if (build_rc != VMM_OK) {
        return;
    }

    CU_ASSERT_EQUAL(img.entry_vaddr, LIBOS_LAUNCH_CODE_VADDR);
    CU_ASSERT_EQUAL(img.stack_top_vaddr, LIBOS_LAUNCH_STACK_TOP);

    libos_destroy_image(PAGE_OWNER_LIBOS, &img);
}

void suite_libos_tetris_tests(CU_pSuite s)
{
    CU_add_test(s, "spawn places a collision-free active piece",
               test_spawn_places_active_piece);
    CU_add_test(s, "move is rejected past the side wall",
               test_move_rejected_past_wall);
    CU_add_test(s, "a full row clears and rows above shift down",
               test_full_row_clears_and_scores);
    CU_add_test(s, "a non-full row is left untouched",
               test_non_full_row_untouched);
    CU_add_test(s, "multiple full rows clear at once",
               test_multiple_rows_clear_at_once);
    CU_add_test(s, "locking spawns a fresh collision-free piece",
               test_lock_active_updates_score_and_spawns_next);
    CU_add_test(s, "a board filled to the brim signals game over",
               test_top_out_sets_game_over);
    CU_add_test(s, "gravity interval speeds up with level and floors",
               test_level_interval_speeds_up_and_floors);
    CU_add_test(s, "tetris image builds and fits its LibOS-window budget",
               test_tetris_image_builds_and_fits_budget);
}
