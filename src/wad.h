#pragma once
#include <stdint.h>

typedef struct {
    const uint8_t *data;
    uint32_t       data_size;  /* total WAD byte length, for bounds checks */
    uint32_t       numlumps;
    const uint8_t *dir_data;  /* raw bytes of the lump directory (16 bytes per entry) */
} wad_t;

/* Returns 0 on success, -1 if magic is wrong or directory is out of bounds. */
int wad_init(wad_t *wad, const uint8_t *data, uint32_t size);

/* Linear scan by name, returns FIRST match.
   For PWAD override semantics, callers must scan manually or use wad_first_flat.
   Sets *size_out (if non-NULL) to lump size.
   Returns pointer into WAD memory, or NULL if not found or out of bounds. */
const uint8_t *wad_find_lump(const wad_t *wad, const char *name, uint32_t *size_out);

/* Returns pointer to first 4096-byte lump between F_START and F_END.
   Skips zero-size namespace marker lumps (e.g. F1_START) intentionally.
   Copies null-terminated name (<=8 chars) into name_out[9].
   Returns NULL if no valid flat is found. */
const uint8_t *wad_first_flat(const wad_t *wad, char name_out[9]);
