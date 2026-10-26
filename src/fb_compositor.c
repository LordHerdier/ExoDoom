#include "fb_compositor.h"
#include "fb_binding.h"
#include "fb_shadow.h"
#include "fb_layout.h"
#include "fb_copy.h"
#include "context.h"
#include "pit.h"

#include <stdint.h>

/* Full-screen path: today's behaviour, one context over the whole screen. */
static void composite_fullscreen(const fb_geometry_t *geom, page_owner_t who) {
    uint64_t shadow_phys;
    if (fb_shadow_lookup(who, &shadow_phys) != 0) {
        return;
    }

    uint64_t fb_bytes = (uint64_t)geom->pitch * (uint64_t)geom->height;

    /* Both extents are inside the kernel's identity map (src/vmm.c always
     * maps the framebuffer aperture; shadow pages come from the ordinary
     * PMM, mapped 1:1 like every other page the kernel manages), so the
     * physical addresses are directly dereferenceable from kernel context. */
    fb_copy_bytes((void *)(uintptr_t)geom->phys_addr,
                  (const void *)(uintptr_t)shadow_phys,
                  (size_t)fb_bytes);
}

/* Split path (SCRUM-197): the panes in the layout table, one row at a time.
 * `only` restricts the pass to one context's pane; PAGE_OWNER_FREE means all.
 * Only the running context draws, so after a full pass the other panes
 * already hold their latest frame. */
static void composite_panes(const fb_geometry_t *geom, page_owner_t only) {
    uint32_t bytes_pp = (uint32_t)(geom->bpp / 8u);

    for (int i = 0; ; i++) {
        page_owner_t who;
        fb_rect_t    rect;
        if (fb_layout_pane(i, &who, &rect) != 0) {
            break;
        }
        if (only != PAGE_OWNER_FREE && who != only) {
            continue;
        }

        /* fb_layout_set() validated the rect against the geometry of the
         * day; fb_binding_init() can republish a smaller one since. Clip to
         * the screen as it is now rather than write past the aperture. */
        if (rect.x >= geom->width || rect.y >= geom->height) {
            continue;
        }
        if (rect.w > geom->width - rect.x)  rect.w = geom->width - rect.x;
        if (rect.h > geom->height - rect.y) rect.h = geom->height - rect.y;

        /* A context that has not called exo_fb_acquire yet shows as the
         * cleared background until it does. */
        fb_shadow_info_t src;
        if (fb_shadow_lookup_info(who, &src) != 0) {
            continue;
        }

        /* The buffer is normally exactly pane-sized (fb_shadow_acquire sizes
         * it from the pane), but one acquired before its pane was assigned,
         * or under a since-moved pane, can differ: clip to the smaller of the
         * two rather than read or write past either. */
        uint32_t w = rect.w < src.width  ? rect.w : src.width;
        uint32_t h = rect.h < src.height ? rect.h : src.height;
        size_t row_bytes = (size_t)w * bytes_pp;

        uint8_t       *dst = (uint8_t *)(uintptr_t)geom->phys_addr +
                             (size_t)rect.y * geom->pitch +
                             (size_t)rect.x * bytes_pp;
        const uint8_t *row = (const uint8_t *)(uintptr_t)src.phys_base;

        for (uint32_t y = 0; y < h; y++) {
            fb_copy_bytes(dst, row, row_bytes);
            dst += geom->pitch;
            row += src.pitch;
        }
    }
}

void fb_compositor_tick(void) {
    /* Remembered across ticks so the screen around the panes is cleared once
     * per change, not copied over every 16 ms. */
    static int      was_split;
    static uint32_t last_generation;

    const fb_geometry_t *geom = fb_binding_geometry();
    if (geom == NULL) {
        return;
    }

    page_owner_t current = context_current();
    fb_rect_t    pane;

    /* No split, or the running context is not in it (the shell, wadview...):
     * exactly the pre-SCRUM-197 full-screen copy. */
    if (fb_layout_count() == 0 || fb_layout_get(current, &pane) != 0) {
        was_split = 0;
        composite_fullscreen(geom, current);
        return;
    }

    uint32_t generation = fb_layout_generation();
    if (!was_split || generation != last_generation) {
        fb_zero_bytes((void *)(uintptr_t)geom->phys_addr,
                      (size_t)geom->pitch * (size_t)geom->height);
        was_split = 1;
        last_generation = generation;
        composite_panes(geom, PAGE_OWNER_FREE);
        return;
    }

    composite_panes(geom, current);
}

void fb_compositor_service(void) {
    if (!pit_take_composite_pending()) {
        return;
    }

    /* SCRUM-181: this runs from ordinary syscall-dispatch context
     * (src/syscall.c) instead of from inside irq0_handler(), which is the
     * point -- the ISR itself now only ever sets a flag and sends EOI, no
     * longer holding a hardware IDT gate (and the PIT's own EOI) behind a
     * multi-MB memcpy.
     *
     * IF stays off for this call, same as the rest of the syscall it runs
     * inside of -- it must NOT be re-enabled here. context.h documents that
     * context_switch_request()'s staging through the single
     * context_switch_out_regs/_in_regs/_pending globals (SCRUM-108) is only
     * correct "because IA32_FMASK clears IF for the whole syscall/switch
     * window ... so no second entry can interleave"; src/ps2.c's Ctrl+Tab
     * IRQ1 handler leans on exactly that guarantee too (its own comment:
     * every *other* context_switch_request() caller is itself a syscall
     * handler, so "there was never a window for a second request to land on
     * top of an unconsumed one"). Re-enabling IF here would open exactly
     * that window mid-dispatch: an IRQ1 Ctrl+Tab could arm a switch, then
     * this same syscall's own handler (sys_yield, for one, which calls
     * context_switch_request() with no pending-guard at all) could arm a
     * second one on top of it, clobbering the first's saved-register
     * pointers -- silent register corruption and a resume into the wrong
     * context. Getting the memcpy out of the ISR is this ticket's fix;
     * making it preemptible too would need context_switch_request() itself
     * to be reentrancy-safe first, which it isn't yet. */
    fb_compositor_tick();
}
