#include "fb_layout.h"
#include "fb_binding.h"
#include "fb_shadow.h"

#include <stddef.h>

typedef struct {
    int          present;
    page_owner_t who;
    fb_rect_t    rect;
} fb_layout_slot_t;

static fb_layout_slot_t slots[FB_LAYOUT_MAX_PANES];
static uint32_t         generation;

static fb_layout_slot_t *find_slot(page_owner_t who) {
    for (int i = 0; i < FB_LAYOUT_MAX_PANES; i++) {
        if (slots[i].present && slots[i].who == who) {
            return &slots[i];
        }
    }
    return NULL;
}

int fb_layout_set(page_owner_t who, const fb_rect_t *rect) {
    const fb_geometry_t *geom = fb_binding_geometry();
    if (geom == NULL || rect == NULL || geom->bpp == 0 || geom->bpp % 8 != 0) {
        return FB_LAYOUT_EINVAL;
    }
    if (rect->w == 0 || rect->h == 0) {
        return FB_LAYOUT_EINVAL;
    }
    /* 64-bit sums: x + w can wrap a uint32_t. */
    if ((uint64_t)rect->x + rect->w > geom->width ||
        (uint64_t)rect->y + rect->h > geom->height) {
        return FB_LAYOUT_EINVAL;
    }

    /* The shadow was sized when it was acquired and cannot be resized, so a
     * pane of another size would be silently clipped or padded. */
    fb_shadow_info_t shadow;
    if (fb_shadow_lookup_info(who, &shadow) == 0 &&
        (shadow.width != rect->w || shadow.height != rect->h)) {
        return FB_LAYOUT_EBUSY;
    }

    fb_layout_slot_t *slot = find_slot(who);
    if (slot == NULL) {
        for (int i = 0; i < FB_LAYOUT_MAX_PANES; i++) {
            if (!slots[i].present) {
                slot = &slots[i];
                break;
            }
        }
        if (slot == NULL) {
            return FB_LAYOUT_ENOSPC;
        }
        slot->present = 1;
        slot->who     = who;
    }

    slot->rect = *rect;
    generation++;
    return FB_LAYOUT_OK;
}

int fb_layout_get(page_owner_t who, fb_rect_t *rect_out) {
    fb_layout_slot_t *slot = find_slot(who);
    if (slot == NULL) {
        return -1;
    }
    *rect_out = slot->rect;
    return 0;
}

void fb_layout_clear(page_owner_t who) {
    fb_layout_slot_t *slot = find_slot(who);
    if (slot == NULL) {
        return;
    }
    slot->present = 0;
    slot->who     = PAGE_OWNER_FREE;
    generation++;
}

int fb_layout_count(void) {
    int n = 0;
    for (int i = 0; i < FB_LAYOUT_MAX_PANES; i++) {
        if (slots[i].present) {
            n++;
        }
    }
    return n;
}

int fb_layout_pane(int index, page_owner_t *who_out, fb_rect_t *rect_out) {
    if (index < 0) {
        return -1;
    }
    for (int i = 0; i < FB_LAYOUT_MAX_PANES; i++) {
        if (!slots[i].present) {
            continue;
        }
        if (index-- == 0) {
            *who_out  = slots[i].who;
            *rect_out = slots[i].rect;
            return 0;
        }
    }
    return -1;
}

uint32_t fb_layout_generation(void) {
    return generation;
}

void fb_layout_reset(void) {
    for (int i = 0; i < FB_LAYOUT_MAX_PANES; i++) {
        slots[i].present = 0;
        slots[i].who     = PAGE_OWNER_FREE;
    }
    generation++;
}
