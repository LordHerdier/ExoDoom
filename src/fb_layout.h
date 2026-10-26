#pragma once
#include <stdint.h>

#include "page_alloc.h"   /* page_owner_t */

/*
 * fb_layout.h — where each context's pane sits on the real screen
 * (SCRUM-197, epic SCRUM-195).
 *
 * Until now the compositor (src/fb_compositor.c) showed exactly one context,
 * full-screen. Two Doom instances need to be visible at once, and the layout
 * is meant to generalise to tmux-style arbitrary splits, so the "which
 * context goes where" decision lives in this kernel-owned table rather than
 * in a per-launch parameter: a launch-time rect is frozen when the app
 * starts, while a pane can be created, moved and dropped at runtime.
 *
 * A pane is a context id plus a rect in real-framebuffer pixels. Two things
 * read it:
 *
 *   - fb_shadow_acquire() (src/fb_shadow.c) sizes a context's virtual
 *     framebuffer to its pane, so exo_fb_acquire reports the pane's
 *     width/height/pitch and the app (Doom's existing nearest-neighbour
 *     scaler, say) draws at exactly that size. The compositor then moves it
 *     with a plain per-row copy -- no per-pixel scaling in the -mno-sse
 *     kernel (src/fb_copy.h).
 *   - fb_compositor_tick() walks the table and blits every pane.
 *
 * Because the shadow is sized when it is first acquired, a pane has to be
 * assigned BEFORE the context's exo_fb_acquire. Once the context holds a
 * shadow its pane may still move, but not change size: fb_layout_set()
 * refuses with FB_LAYOUT_EBUSY rather than leave a buffer that does not
 * match its pane. Live resize is a separate story.
 *
 * Concurrency: the table has no lock. Its writers (exo_exit, revocation,
 * context_destroy) and its reader (fb_compositor_tick, from syscall
 * dispatch) all run with IF clear, so they cannot interleave on one CPU. A
 * future caller that can run with interrupts enabled needs a lock first.
 *
 * A context with no pane is not part of the split: the compositor falls
 * back to showing context_current() full-screen when that context has none,
 * so the shell, wadview and friends are unaffected and the shell stays
 * reachable.
 *
 * Panes may overlap; they are drawn in slot order, lowest slot first, so the
 * higher slot lands on top. A free slot is reused first-fit, so depth follows
 * slot index and NOT assignment time: clear a low pane and assign a new one
 * and the new pane takes the low slot, underneath the others. (Re-setting a
 * context's pane keeps its slot, and with it its depth.)
 *
 * Like fb_shadow.h this module is ABI-agnostic: it speaks FB_LAYOUT_*
 * status codes, and nothing exposes it to ring 3 yet.
 */

typedef struct {
    uint32_t x;   /* left edge, pixels from the screen's left     */
    uint32_t y;   /* top edge, pixels from the screen's top       */
    uint32_t w;   /* width, pixels                                */
    uint32_t h;   /* height, pixels                               */
} fb_rect_t;

/* More than the two Doom panes the epic needs, small enough to walk every
 * 60 Hz tick without thinking about it. */
#define FB_LAYOUT_MAX_PANES 8

#define FB_LAYOUT_OK       0
#define FB_LAYOUT_EINVAL (-1)  /* no framebuffer, empty rect, or off-screen   */
#define FB_LAYOUT_ENOSPC (-2)  /* FB_LAYOUT_MAX_PANES panes already assigned  */
#define FB_LAYOUT_EBUSY  (-3)  /* owner already holds a shadow of another size */

/*
 * Give `who` the pane `rect`, or move it if it already has one. Validated
 * against fb_binding_geometry(): the rect must be non-empty and lie wholly
 * on the published screen, and the framebuffer must be a whole number of
 * bytes per pixel. FB_LAYOUT_EINVAL otherwise (nothing changes). If `who`
 * already holds a virtual framebuffer, the rect must have that buffer's
 * width and height: FB_LAYOUT_EBUSY otherwise (nothing changes).
 */
int fb_layout_set(page_owner_t who, const fb_rect_t *rect);

/* Copy `who`'s pane into *rect_out. 0 if it has one, nonzero if not. */
int fb_layout_get(page_owner_t who, fb_rect_t *rect_out);

/* Drop `who`'s pane. A no-op if it has none, so exit and revocation paths
 * (via fb_shadow_release()) can call it unconditionally. */
void fb_layout_clear(page_owner_t who);

/* Panes currently assigned. 0 means "no split": the compositor shows
 * context_current() full-screen exactly as it did before SCRUM-197. */
int fb_layout_count(void);

/*
 * Pane number `index` (0 .. fb_layout_count()-1) in draw order, i.e. the
 * lowest one first. 0 and fills the outputs, or nonzero if out of range.
 */
int fb_layout_pane(int index, page_owner_t *who_out, fb_rect_t *rect_out);

/* Bumped on every change to the table, so the compositor can tell that the
 * screen around the panes needs clearing again. */
uint32_t fb_layout_generation(void);

/* Drop every pane. For tests and for tearing a split down wholesale. */
void fb_layout_reset(void);
