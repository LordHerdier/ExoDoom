#include "fb_shadow.h"
#include "fb_binding.h"
#include "fb_copy.h"
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
        uint64_t fb_bytes = (uint64_t)geom->pitch * (uint64_t)geom->height;
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
    }

    info_out->phys_addr = slot->phys_base;
    info_out->width     = geom->width;
    info_out->height    = geom->height;
    info_out->pitch     = geom->pitch;
    info_out->bpp       = geom->bpp;
    info_out->reserved[0] = 0;
    info_out->reserved[1] = 0;
    info_out->reserved[2] = 0;

    return FB_SHADOW_OK;
}

int fb_shadow_acquire(page_owner_t who, exo_fb_info_t *info_out) {
    return shadow_acquire(who, info_out, 1);
}

int fb_shadow_seed_from_screen(page_owner_t who) {
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
