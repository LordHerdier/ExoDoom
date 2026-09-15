#include "automap.h"
#include "fb.h"
#include "wad.h"

/* ── Doom map binary structures ─────────────────────────────────────────── */

typedef struct {
    int16_t x;
    int16_t y;
} doom_vertex_t;

typedef struct {
    uint16_t v1;
    uint16_t v2;
    uint16_t flags;
    uint16_t special;
    uint16_t tag;
    uint16_t sidenum[2]; /* 0xFFFF = no second side (one-sided) */
} doom_linedef_t;

typedef struct {
    int16_t  x;
    int16_t  y;
    uint16_t angle;
    uint16_t type;
    uint16_t flags;
} doom_thing_t;

#define ML_SECRET 0x0020

/* ── Helpers ────────────────────────────────────────────────────────────── */

static inline int16_t read_i16(const uint8_t *p) {
    uint16_t v;
    __builtin_memcpy(&v, p, 2);
    return (int16_t)v;
}

static inline uint16_t read_u16(const uint8_t *p) {
    uint16_t v;
    __builtin_memcpy(&v, p, 2);
    return v;
}

/* ── Render ─────────────────────────────────────────────────────────────── */

int automap_render(framebuffer_t *fb, const wad_t *wad, const char *map_name,
                   uint32_t margin) {
    if (!fb || !wad || !map_name) return -1;

    /* Fetch VERTEXES */
    uint32_t verts_size = 0;
    const uint8_t *verts_raw = wad_find_map_lump(wad, map_name, "VERTEXES", &verts_size);
    if (!verts_raw || verts_size < 4) return -1;
    uint32_t num_verts = verts_size / 4; /* each vertex is 4 bytes */

    /* Fetch LINEDEFS */
    uint32_t lines_size = 0;
    const uint8_t *lines_raw = wad_find_map_lump(wad, map_name, "LINEDEFS", &lines_size);
    if (!lines_raw || lines_size < 14) return -1;
    uint32_t num_lines = lines_size / 14; /* each linedef is 14 bytes */

    /* Fetch THINGS (optional — we still draw the map without them) */
    uint32_t things_size = 0;
    const uint8_t *things_raw = wad_find_map_lump(wad, map_name, "THINGS", &things_size);
    uint32_t num_things = things_raw ? things_size / 10 : 0; /* 10 bytes each */

    /* ── Find map bounding box ──────────────────────────────────────────── */
    int32_t min_x = 32767, max_x = -32768;
    int32_t min_y = 32767, max_y = -32768;

    for (uint32_t i = 0; i < num_verts; i++) {
        int16_t vx = read_i16(verts_raw + i * 4);
        int16_t vy = read_i16(verts_raw + i * 4 + 2);
        if (vx < min_x) min_x = vx;
        if (vx > max_x) max_x = vx;
        if (vy < min_y) min_y = vy;
        if (vy > max_y) max_y = vy;
    }

    int32_t map_w = max_x - min_x;
    int32_t map_h = max_y - min_y;
    if (map_w <= 0 || map_h <= 0) return -1;

    /* ── Compute scale to fit screen ────────────────────────────────────── */
    uint32_t screen_w = fb->width  - 2 * margin;
    uint32_t screen_h = fb->height - 2 * margin;

    /* Use fixed-point scale (16.16) to avoid floating point */
    uint32_t scale_x = (screen_w << 16) / (uint32_t)map_w;
    uint32_t scale_y = (screen_h << 16) / (uint32_t)map_h;
    uint32_t scale = (scale_x < scale_y) ? scale_x : scale_y;

    /* Center the map */
    uint32_t drawn_w = ((uint32_t)map_w * scale) >> 16;
    uint32_t drawn_h = ((uint32_t)map_h * scale) >> 16;
    int32_t off_x = (int32_t)(margin + (screen_w - drawn_w) / 2);
    int32_t off_y = (int32_t)(margin + (screen_h - drawn_h) / 2);

    /* Transform a map vertex to screen coordinates.
     * Doom's Y axis points up; screen Y points down, so we flip. */
    #define MAP_TO_SCREEN_X(vx) (off_x + (int32_t)((((uint32_t)((vx) - min_x)) * scale) >> 16))
    #define MAP_TO_SCREEN_Y(vy) (off_y + (int32_t)(drawn_h - ((((uint32_t)((vy) - min_y)) * scale) >> 16)))

    /* ── Draw LINEDEFS ──────────────────────────────────────────────────── */
    for (uint32_t i = 0; i < num_lines; i++) {
        const uint8_t *ld = lines_raw + i * 14;
        uint16_t v1_idx = read_u16(ld);
        uint16_t v2_idx = read_u16(ld + 2);
        uint16_t flags  = read_u16(ld + 4);
        uint16_t side1  = read_u16(ld + 10);
        uint16_t side2  = read_u16(ld + 12);

        if (v1_idx >= num_verts || v2_idx >= num_verts) continue;

        int16_t x0 = read_i16(verts_raw + v1_idx * 4);
        int16_t y0 = read_i16(verts_raw + v1_idx * 4 + 2);
        int16_t x1 = read_i16(verts_raw + v2_idx * 4);
        int16_t y1 = read_i16(verts_raw + v2_idx * 4 + 2);

        int32_t sx0 = MAP_TO_SCREEN_X(x0);
        int32_t sy0 = MAP_TO_SCREEN_Y(y0);
        int32_t sx1 = MAP_TO_SCREEN_X(x1);
        int32_t sy1 = MAP_TO_SCREEN_Y(y1);

        uint8_t r, g, b;
        if (flags & ML_SECRET) {
            r = 200; g = 0; b = 200; /* magenta — secret */
        } else if (side2 == 0xFFFF || side1 == 0xFFFF) {
            r = 200; g = 40; b = 40; /* red — solid wall */
        } else {
            r = 100; g = 100; b = 100; /* gray — two-sided */
        }

        fb_draw_line(fb, sx0, sy0, sx1, sy1, r, g, b);
    }

    /* ── Draw THINGS ────────────────────────────────────────────────────── */
    for (uint32_t i = 0; i < num_things; i++) {
        const uint8_t *th = things_raw + i * 10;
        int16_t tx = read_i16(th);
        int16_t ty = read_i16(th + 2);
        uint16_t type = read_u16(th + 6);

        int32_t sx = MAP_TO_SCREEN_X(tx);
        int32_t sy = MAP_TO_SCREEN_Y(ty);

        uint8_t r, g, b;
        if (type == 1) {
            /* Player 1 start — bright green, larger dot */
            r = 0; g = 255; b = 0;
            for (int dy = -3; dy <= 3; dy++)
                for (int dx = -3; dx <= 3; dx++)
                    fb_put_pixel(fb, sx + dx, sy + dy, r, g, b);
            continue;
        } else if (type == 2 || type == 3 || type == 4) {
            /* Player 2-4 starts — dimmer green */
            r = 0; g = 160; b = 0;
        } else if (type >= 2001 && type <= 2006) {
            /* Weapons — cyan */
            r = 0; g = 180; b = 220;
        } else if ((type >= 5 && type <= 45) || type == 88 ||
                   (type >= 64 && type <= 72)) {
            /* Monsters — red */
            r = 220; g = 50; b = 50;
        } else {
            /* Items/misc — yellow */
            r = 200; g = 200; b = 40;
        }

        /* Small 3x3 dot */
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
                fb_put_pixel(fb, sx + dx, sy + dy, r, g, b);
    }

    #undef MAP_TO_SCREEN_X
    #undef MAP_TO_SCREEN_Y

    return 0;
}
