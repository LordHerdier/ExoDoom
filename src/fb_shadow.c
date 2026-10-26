#include "fb_shadow.h"
#include "fb_binding.h"
#include "fb_copy.h"
#include "fb_layout.h"
#include "page_alloc.h"
#include "vmm.h"
#include "string.h"

#include <stddef.h>

#define PAGE_SIZE 4096ULL

typedef struct {
    int          present;
    page_owner_t who;
    uint64_t     phys_base;
    uint32_t     page_count;
    /* What this buffer was sized as: the context's pane (src/fb_layout.h) if
     * it had one when it first acquired, else the whole screen. Kept per slot
     * so a re-acquire reports the buffer that exists, and the compositor
     * knows the source pitch. */
    uint32_t     width;
    uint32_t     height;
    uint32_t     pitch;
    uint8_t      bpp;
} fb_shadow_slot_t;

/* See this module's header comment for why VMM_MAX_ADDRESS_SPACES rather
 * than context.c's smaller CONTEXT_MAX. */
static fb_shadow_slot_t slots[VMM_MAX_ADDRESS_SPACES];

static fb_shadow_slot_t *find_slot(page_owner_t who) {
    for (int i = 0; i < VMM_MAX_ADDRESS_SPACES; i++) {
        if (slots[i].present && slots[i].who == who) {
            return &slots[i];
        }
    }
    return NULL;
}

static fb_shadow_slot_t *find_free_slot(void) {
    for (int i = 0; i < VMM_MAX_ADDRESS_SPACES; i++) {
        if (!slots[i].present) {
            return &slots[i];
        }
    }
    return NULL;
}

/* `zero` is 0 only for fb_shadow_seed_from_screen(), which overwrites the
 * whole buffer straight afterwards. */
static int shadow_acquire(page_owner_t who, exo_fb_info_t *info_out,
                          int zero) {
    const fb_geometry_t *geom = fb_binding_geometry();
    if (geom == NULL) {
        return FB_SHADOW_ENODEV;
    }

    fb_shadow_slot_t *slot = find_slot(who);

    if (slot == NULL) {
        /* SCRUM-197: a context with a pane gets a buffer exactly its pane's
         * size, tightly pitched, so the compositor's copy is a plain row-by-
         * row move and an app scaling to fb.width/height fills the pane. */
        uint32_t width = geom->width, height = geom->height;
        uint32_t pitch = geom->pitch;
        fb_rect_t pane;
        if (fb_layout_get(who, &pane) == 0) {
            width  = pane.w;
            height = pane.h;
            pitch  = pane.w * (uint32_t)(geom->bpp / 8u);
        }

        uint64_t fb_bytes = (uint64_t)pitch * (uint64_t)height;
        uint32_t page_count = (uint32_t)((fb_bytes + PAGE_SIZE - 1) / PAGE_SIZE);

        void *base = alloc_pages_contig_owned(who, page_count);
        if (base == NULL) {
            return FB_SHADOW_ENOMEM;
        }

        slot = find_free_slot();
        if (slot == NULL) {
            /* Cannot happen in practice: a context needs a live address
             * space (bounded by VMM_MAX_ADDRESS_SPACES) to have a `who`
             * worth allocating for in the first place — see this module's
             * header comment. Free the pages rather than leak them if it
             * ever does. */
            for (uint32_t i = 0; i < page_count; i++) {
                free_page_owned((void *)(uintptr_t)((uint64_t)(uintptr_t)base +
                                                    i * PAGE_SIZE), who);
            }
            return FB_SHADOW_ENOMEM;
        }

        if (zero) {
            fb_zero_bytes(base, (size_t)page_count * PAGE_SIZE);
        }

        slot->present    = 1;
        slot->who        = who;
        slot->phys_base  = (uint64_t)(uintptr_t)base;
        slot->page_count = page_count;
        slot->width      = width;
        slot->height     = height;
        slot->pitch      = pitch;
        slot->bpp        = geom->bpp;
    }

    info_out->phys_addr = slot->phys_base;
    info_out->width     = slot->width;
    info_out->height    = slot->height;
    info_out->pitch     = slot->pitch;
    info_out->bpp       = slot->bpp;
    info_out->reserved[0] = 0;
    info_out->reserved[1] = 0;
    info_out->reserved[2] = 0;

    return FB_SHADOW_OK;
}

int fb_shadow_acquire(page_owner_t who, exo_fb_info_t *info_out) {
    return shadow_acquire(who, info_out, 1);
}

int fb_shadow_seed_from_screen(page_owner_t who) {
    /* A pane-sized buffer cannot hold the whole screen: seed it with the
     * screen's own pixels under the pane instead (SCRUM-197), so a split
     * context has no black first frame either. */
    fb_rect_t pane;
    if (fb_layout_get(who, &pane) == 0) {
        exo_fb_info_t info;
        int rc = shadow_acquire(who, &info, 1);
        const fb_geometry_t *g = fb_binding_geometry();
        if (rc != FB_SHADOW_OK || g == NULL ||
            pane.x >= g->width || pane.y >= g->height) {
            return rc;
        }

        uint32_t bytes_pp = (uint32_t)(g->bpp / 8u);
        uint32_t w = info.width  < g->width  - pane.x ? info.width  : g->width  - pane.x;
        uint32_t h = info.height < g->height - pane.y ? info.height : g->height - pane.y;

        uint8_t       *dst = (uint8_t *)(uintptr_t)info.phys_addr;
        const uint8_t *src = (const uint8_t *)(uintptr_t)g->phys_addr +
                             (size_t)pane.y * g->pitch +
                             (size_t)pane.x * bytes_pp;
        for (uint32_t y = 0; y < h; y++) {
            fb_copy_bytes(dst, src, (size_t)w * bytes_pp);
            dst += info.pitch;
            src += g->pitch;
        }
        return FB_SHADOW_OK;
    }

    exo_fb_info_t info;
    int rc = shadow_acquire(who, &info, 0);
    if (rc != FB_SHADOW_OK) {
        return rc;
    }

    /* Non-NULL: fb_shadow_acquire() just succeeded off the same geometry. */
    const fb_geometry_t *geom = fb_binding_geometry();
    uint64_t fb_bytes = (uint64_t)geom->pitch * (uint64_t)geom->height;

    fb_copy_bytes((void *)(uintptr_t)info.phys_addr,
                  (const void *)(uintptr_t)geom->phys_addr,
                  (size_t)fb_bytes);
    return FB_SHADOW_OK;
}

void fb_shadow_release(page_owner_t who) {
    /* Before the early return: a pane can be assigned to a context that never
     * acquired, and must not outlive it either (SCRUM-197). */
    fb_layout_clear(who);

    fb_shadow_slot_t *slot = find_slot(who);
    if (slot == NULL) {
        return;
    }

    /* SCRUM-187: free exactly the pages this slot allocated, rather than
     * leaving that to a caller-side reclaim_pages_owned(who) sweep — such a
     * sweep frees *every* page `who` owns, not just this buffer's, and
     * collides with anything else that happens to share the same owner id
     * (see this file's header comment in fb_shadow.h). Same per-page loop
     * fb_shadow_acquire()'s own ENOMEM cleanup path already uses above. */
    for (uint32_t i = 0; i < slot->page_count; i++) {
        void *page = (void *)(uintptr_t)(slot->phys_base + (uint64_t)i * PAGE_SIZE);
        free_page_owned(page, who);
    }

    slot->present    = 0;
    slot->who        = PAGE_OWNER_FREE;
    slot->phys_base  = 0;
    slot->page_count = 0;
}

int fb_shadow_lookup(page_owner_t who, uint64_t *phys_base_out) {
    fb_shadow_slot_t *slot = find_slot(who);
    if (slot == NULL) {
        return -1;
    }

    *phys_base_out = slot->phys_base;
    return 0;
}

int fb_shadow_lookup_info(page_owner_t who, fb_shadow_info_t *info_out) {
    fb_shadow_slot_t *slot = find_slot(who);
    if (slot == NULL) {
        return -1;
    }

    info_out->phys_base = slot->phys_base;
    info_out->width     = slot->width;
    info_out->height    = slot->height;
    info_out->pitch     = slot->pitch;
    return 0;
}
