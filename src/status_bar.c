#include "status_bar.h"
#include "fb.h"
#include "font8x8.h"
#include "string.h"

#include <stddef.h>

#define BAR_BG_R 0x1c
#define BAR_BG_G 0x24
#define BAR_BG_B 0x34
#define BAR_FG_R 0xd8
#define BAR_FG_G 0xd8
#define BAR_FG_B 0xd8

static int      enabled;
static int      drawn_valid;
static int      drawn_enabled;
static uint64_t drawn_base;
static uint32_t drawn_pitch;
static uint32_t drawn_width;
static char     drawn_line[STATUS_BAR_MAX_COLS + 1];
static uint32_t drawn_hl_start, drawn_hl_end;

/* What the entries looked like on the last update, to notice a change and
 * restart the tooltip. */
static char     seen_entries[STATUS_BAR_MAX_COLS + 1];
static uint32_t seen_hl_start, seen_hl_end;
static int      seen_valid;
static uint32_t tooltip_until_ms;

static uint32_t put_str(char *line, uint32_t pos, uint32_t cap, const char *s) {
    while (*s != '\0' && pos < cap) {
        line[pos++] = *s++;
    }
    return pos;
}

static uint32_t put_num(char *line, uint32_t pos, uint32_t cap, uint32_t v) {
    char tmp[10];
    int  len = 0;
    do {
        tmp[len++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0 && len < (int)sizeof(tmp));
    while (len > 0 && pos < cap) {
        line[pos++] = tmp[--len];
    }
    return pos;
}

static int is_live(const context_info_t *c) {
    return c->state == CONTEXT_STATE_READY || c->state == CONTEXT_STATE_RUNNING;
}

void status_bar_format(const context_info_t *ctx, uint32_t n,
                       page_owner_t focused, const char *right,
                       uint32_t cols, char *line,
                       uint32_t *hl_start, uint32_t *hl_end) {
    if (cols > STATUS_BAR_MAX_COLS) {
        cols = STATUS_BAR_MAX_COLS;
    }
    memset(line, ' ', cols);
    line[cols] = '\0';
    *hl_start = 0;
    *hl_end = 0;

    uint32_t right_len = 0;
    while (right != NULL && right[right_len] != '\0') {
        right_len++;
    }
    if (right_len > cols) {
        right_len = cols;
    }

    /* One blank column between the entries and the right-hand text. */
    uint32_t cap = cols > right_len + 1u ? cols - right_len - 1u : 0u;
    uint32_t pos = 0;
    uint32_t index = 0;
    int      cut = 0;

    for (uint32_t i = 0; i < n && !cut; i++) {
        if (!is_live(&ctx[i])) {
            continue;
        }

        char     entry[CONTEXT_NAME_MAX + 8];
        uint32_t len = 0;
        int      focus = ctx[i].id == focused;

        if (focus) entry[len++] = '[';
        len = put_num(entry, len, sizeof(entry) - 2u, index);
        entry[len++] = ':';
        len = put_str(entry, len, sizeof(entry) - 2u,
                      ctx[i].name[0] != '\0' ? ctx[i].name : "?");
        if (focus) entry[len++] = ']';
        index++;

        uint32_t gap = pos == 0 ? 0u : 2u;
        if (pos + gap + len > cap) {
            cut = 1;
            /* Mark the cut; reuse the tail of what already fits. */
            if (cap >= 2u) {
                uint32_t at = pos + gap <= cap - 2u ? pos + gap : cap - 2u;
                line[at] = '.';
                line[at + 1u] = '.';
            }
            break;
        }

        pos += gap;
        memcpy(line + pos, entry, len);
        if (focus) {
            *hl_start = pos;
            *hl_end = pos + len;
        }
        pos += len;
    }

    if (right_len != 0) {
        memcpy(line + (cols - right_len), right, right_len);
    }
}

void status_bar_set_enabled(int on) {
    enabled = on != 0;
}

int status_bar_enabled(void) {
    return enabled;
}

void status_bar_reset(void) {
    enabled = 0;
    drawn_valid = 0;
    seen_valid = 0;
    tooltip_until_ms = 0;
}

static void draw_cell(framebuffer_t *fb, uint32_t col, uint32_t y, char ch,
                      int inverse) {
    uint8_t bg_r = inverse ? BAR_FG_R : BAR_BG_R;
    uint8_t bg_g = inverse ? BAR_FG_G : BAR_BG_G;
    uint8_t bg_b = inverse ? BAR_FG_B : BAR_BG_B;
    uint8_t fg_r = inverse ? BAR_BG_R : BAR_FG_R;
    uint8_t fg_g = inverse ? BAR_BG_G : BAR_FG_G;
    uint8_t fg_b = inverse ? BAR_BG_B : BAR_FG_B;

    uint32_t x = col * 8u;
    fb_fill_rect(fb, x, y, 8, FB_BAR_ROWS, bg_r, bg_g, bg_b);

    if (ch <= ' ' || (uint8_t)ch > 126) {
        return;
    }
    const uint8_t *g = font8x8_basic[(uint8_t)ch];
    for (uint32_t row = 0; row < 8; row++) {
        for (uint32_t b = 0; b < 8; b++) {
            if (g[row] & (1u << b)) {
                fb_fill_rect(fb, x + b, y + row * 2u, 1, 2, fg_r, fg_g, fg_b);
            }
        }
    }
}

void status_bar_update(const fb_geometry_t *geom, uint32_t now_ms) {
    if (geom == NULL || !fb_binding_has_bar()) {
        return;
    }

    framebuffer_t fb;
    if (!fb_init_bgrx8888(&fb, (uintptr_t)geom->phys_addr, geom->pitch,
                          geom->width, geom->height, geom->bpp)) {
        return;
    }
    uint32_t y = geom->height - FB_BAR_ROWS;

    int geometry_same = drawn_valid && drawn_base == geom->phys_addr &&
                        drawn_pitch == geom->pitch && drawn_width == geom->width;

    if (!enabled) {
        if (!geometry_same || drawn_enabled) {
            fb_fill_rect(&fb, 0, y, geom->width, FB_BAR_ROWS,
                         0, 0, 0);
        }
        drawn_valid = 1;
        drawn_enabled = 0;
        drawn_base = geom->phys_addr;
        drawn_pitch = geom->pitch;
        drawn_width = geom->width;
        seen_valid = 0;
        return;
    }

    uint32_t cols = geom->width / 8u;
    if (cols > STATUS_BAR_MAX_COLS) {
        cols = STATUS_BAR_MAX_COLS;
    }

    context_info_t ctx[CONTEXT_MAX];
    uint32_t n = context_list(ctx, CONTEXT_MAX);
    page_owner_t focused = context_current();

    /* A change in the entries or the focus restarts the tooltip. */
    char     entries[STATUS_BAR_MAX_COLS + 1];
    uint32_t hs, he;
    status_bar_format(ctx, n, focused, NULL, cols, entries, &hs, &he);
    if (!seen_valid || hs != seen_hl_start || he != seen_hl_end ||
        memcmp(entries, seen_entries, cols + 1u) != 0) {
        tooltip_until_ms = now_ms + STATUS_BAR_TOOLTIP_MS;
        memcpy(seen_entries, entries, cols + 1u);
        seen_hl_start = hs;
        seen_hl_end = he;
        seen_valid = 1;
    }

    int tooltip = (int32_t)(tooltip_until_ms - now_ms) > 0;
    char line[STATUS_BAR_MAX_COLS + 1];
    status_bar_format(ctx, n, focused,
                      tooltip ? STATUS_BAR_TOOLTIP : STATUS_BAR_HINT,
                      cols, line, &hs, &he);

    if (geometry_same && drawn_enabled && hs == drawn_hl_start &&
        he == drawn_hl_end && memcmp(line, drawn_line, cols + 1u) == 0) {
        return;
    }

    fb_fill_rect(&fb, 0, y, geom->width, FB_BAR_ROWS, BAR_BG_R, BAR_BG_G, BAR_BG_B);
    for (uint32_t c = 0; c < cols; c++) {
        draw_cell(&fb, c, y, line[c], c >= hs && c < he);
    }

    memcpy(drawn_line, line, cols + 1u);
    drawn_hl_start = hs;
    drawn_hl_end = he;
    drawn_enabled = 1;
    drawn_valid = 1;
    drawn_base = geom->phys_addr;
    drawn_pitch = geom->pitch;
    drawn_width = geom->width;
}
