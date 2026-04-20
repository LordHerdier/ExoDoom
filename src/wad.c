#include "wad.h"

static inline uint32_t read_le32(const uint8_t *p) {
    uint32_t v;
    __builtin_memcpy(&v, p, 4);
    return v;
}

/* Compare a raw 8-char lump name (not null-terminated) against a C string. */
static int lump_name_eq(const uint8_t *name8, const char *name) {
    for (int i = 0; i < 8; i++) {
        if (name[i] == '\0') {
            for (int j = i; j < 8; j++)
                if (name8[j] != '\0') return 0;
            return 1;
        }
        if (name8[i] != (uint8_t)name[i]) return 0;
    }
    return name[8] == '\0';
}

int wad_init(wad_t *wad, const uint8_t *data, uint32_t size) {
    if (size < 12) return -1;
    if ((data[0] != 'I' && data[0] != 'P') ||
        data[1] != 'W' || data[2] != 'A' || data[3] != 'D')
        return -1;

    uint32_t numlumps = read_le32(data + 4);
    uint32_t dirofs   = read_le32(data + 8);

    if (dirofs < 12) return -1;

    /* Check separately to avoid overflow in numlumps * 16u. */
    if (dirofs > size || numlumps > (size - dirofs) / 16u) return -1;

    wad->data      = data;
    wad->data_size = size;
    wad->numlumps  = numlumps;
    wad->dir_data  = data + dirofs;
    return 0;
}

const uint8_t *wad_find_lump(const wad_t *wad, const char *name, uint32_t *size_out) {
    for (uint32_t i = 0; i < wad->numlumps; i++) {
        const uint8_t *entry = wad->dir_data + i * 16u;
        if (lump_name_eq(entry + 8, name)) {
            uint32_t filepos   = read_le32(entry);
            uint32_t lump_size = read_le32(entry + 4);
            /* Use subtraction form to avoid overflow in filepos + lump_size. */
            if (lump_size > wad->data_size || filepos > wad->data_size - lump_size)
                return (void *)0;
            if (size_out) *size_out = lump_size;
            return wad->data + filepos;
        }
    }
    return (void *)0;
}

const uint8_t *wad_first_flat(const wad_t *wad, char name_out[9]) {
    int in_flats = 0;
    for (uint32_t i = 0; i < wad->numlumps; i++) {
        const uint8_t *entry = wad->dir_data + i * 16u;
        if (lump_name_eq(entry + 8, "F_START")) { in_flats = 1; continue; }
        if (lump_name_eq(entry + 8, "F_END"))   { break; }
        uint32_t lump_size = read_le32(entry + 4);
        if (in_flats && lump_size == 4096) {
            /* size==4096 intentionally skips zero-size namespace markers
               (F1_START, FF_START, etc.) that appear between F_START and F_END. */
            uint32_t filepos = read_le32(entry);
            /* Subtraction form avoids overflow when filepos is near UINT32_MAX. */
            if (4096u > wad->data_size || filepos > wad->data_size - 4096u) continue;
            if (name_out) {
                for (int j = 0; j < 8; j++) name_out[j] = (char)(entry + 8)[j];
                name_out[8] = '\0';
            }
            return wad->data + filepos;
        }
    }
    return (void *)0;
}

uint32_t wad_count_flats(const wad_t *wad) {
    uint32_t count = 0;
    int in_flats = 0;
    for (uint32_t i = 0; i < wad->numlumps; i++) {
        const uint8_t *entry = wad->dir_data + i * 16u;
        if (lump_name_eq(entry + 8, "F_START")) { in_flats = 1; continue; }
        if (lump_name_eq(entry + 8, "F_END"))   { break; }
        if (!in_flats) continue;
        uint32_t lump_size = read_le32(entry + 4);
        if (lump_size != 4096) continue;
        uint32_t filepos = read_le32(entry);
        if (4096u > wad->data_size || filepos > wad->data_size - 4096u) continue;
        count++;
    }
    return count;
}

int wad_find_map(const wad_t *wad, const char *map_name) {
    for (uint32_t i = 0; i < wad->numlumps; i++) {
        const uint8_t *entry = wad->dir_data + i * 16u;
        if (lump_name_eq(entry + 8, map_name))
            return (int)i;
    }
    return -1;
}

const uint8_t *wad_find_map_lump(const wad_t *wad, const char *map_name,
                                  const char *lump_name, uint32_t *size_out) {
    int map_idx = wad_find_map(wad, map_name);
    if (map_idx < 0) return (void *)0;

    /* Map sub-lumps appear within 11 entries after the map marker. */
    uint32_t end = (uint32_t)map_idx + 12;
    if (end > wad->numlumps) end = wad->numlumps;

    for (uint32_t i = (uint32_t)map_idx + 1; i < end; i++) {
        const uint8_t *entry = wad->dir_data + i * 16u;
        if (lump_name_eq(entry + 8, lump_name)) {
            uint32_t filepos   = read_le32(entry);
            uint32_t lump_size = read_le32(entry + 4);
            if (lump_size > wad->data_size || filepos > wad->data_size - lump_size)
                return (void *)0;
            if (size_out) *size_out = lump_size;
            return wad->data + filepos;
        }
    }
    return (void *)0;
}

const uint8_t *wad_get_flat(const wad_t *wad, uint32_t index, char name_out[9]) {
    uint32_t seen = 0;  /* number of valid flats encountered so far */
    int in_flats = 0;
    for (uint32_t i = 0; i < wad->numlumps; i++) {
        const uint8_t *entry = wad->dir_data + i * 16u;
        if (lump_name_eq(entry + 8, "F_START")) { in_flats = 1; continue; }
        if (lump_name_eq(entry + 8, "F_END"))   { break; }
        if (!in_flats) continue;
        uint32_t lump_size = read_le32(entry + 4);
        if (lump_size != 4096) continue;
        uint32_t filepos = read_le32(entry);
        if (4096u > wad->data_size || filepos > wad->data_size - 4096u) continue;
        if (seen == index) {
            if (name_out) {
                const uint8_t *n = entry + 8;
                for (int j = 0; j < 8; j++) name_out[j] = (char)n[j];
                name_out[8] = '\0';
            }
            return wad->data + filepos;
        }
        seen++;
    }
    return (void *)0;
}
