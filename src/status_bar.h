#pragma once

#include "context.h"
#include "fb_binding.h"

#include <stdint.h>

/*
 * status_bar.h -- the tmux-style strip along the bottom of the screen
 * (SCRUM-278). Drawn by the compositor into the FB_BAR_ROWS rows that
 * fb_binding_usable_height() keeps away from every program, so no program
 * ever maps or sees it. It lists the live contexts in context-table order,
 * brackets the focused one, and shows a right-aligned Ctrl+Tab hint.
 *
 * On by default. The rows stay reserved either way: programs size their
 * buffers once, so turning the bar off blanks the strip instead of giving
 * the rows back.
 *
 * "Live" means READY or RUNNING. A launch hold, a context mid-teardown and a
 * stale row are BLOCKED, so the bar never lists a program that cannot run.
 * "Focused" is context_current(), the same value fb_compositor_tick() uses to
 * pick the frame, so the bar and the picture cannot disagree.
 */

#define STATUS_BAR_MAX_COLS      200u   /* 8-pixel cells; wider screens leave the rest blank */
#define STATUS_BAR_HINT          "Ctrl+Tab: next"
#define STATUS_BAR_TOOLTIP       "Ctrl+Tab cycles programs -- Esc/Q quits most apps"
#define STATUS_BAR_TOOLTIP_MS    4000u  /* how long the tooltip replaces the hint */

/*
 * Pure text layout, no hardware. Writes exactly `cols` characters plus a NUL
 * into `line` (capacity cols + 1) and the half-open column range of the
 * focused entry into [*hl_start, *hl_end) (both 0 if none is focused).
 *
 * Entries are `n:name` with n the position among live contexts, separated by
 * two spaces; the focused one is `[n:name]`. `right` is right-aligned and
 * wins the row: if the entries do not fit beside it, they are cut with a
 * trailing `..`. Contexts that are not READY/RUNNING are skipped.
 * `cols` is clamped to STATUS_BAR_MAX_COLS.
 */
void status_bar_format(const context_info_t *ctx, uint32_t n,
                       page_owner_t focused, const char *right,
                       uint32_t cols, char *line,
                       uint32_t *hl_start, uint32_t *hl_end);

/* 0 = off, nonzero = on (the default). */
void status_bar_set_enabled(int on);
int  status_bar_enabled(void);

/*
 * Bring the strip up to date for the frame the compositor just composed.
 * Repaints only when what it would show changed (the entries, the focus, the
 * tooltip phase, the toggle, or the screen); otherwise it costs one table
 * walk and a compare. `now_ms` is kernel_get_ticks_ms(). A no-op without a
 * framebuffer, on a screen with no reserved strip, or on a non-32bpp mode.
 */
void status_bar_update(const fb_geometry_t *geom, uint32_t now_ms);

/* Forget what was last drawn and restore the defaults (on); for tests and
 * for a republished framebuffer geometry. */
void status_bar_reset(void);
