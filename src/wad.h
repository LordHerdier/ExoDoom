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
   If name_out is non-NULL, copies the null-terminated name (<=8 chars) into name_out[9].
   Returns NULL if no valid flat is found. */
const uint8_t *wad_first_flat(const wad_t *wad, char name_out[9]);

/* Count all valid flats (4096-byte lumps) between F_START and F_END. */
uint32_t wad_count_flats(const wad_t *wad);

/* Return the flat at zero-based index among valid flats between F_START and F_END.
   If name_out is non-NULL, copies the null-terminated name (<=8 chars) into name_out[9].
   Returns NULL if index is out of range or the lump is out of bounds. */
const uint8_t *wad_get_flat(const wad_t *wad, uint32_t index, char name_out[9]);

/* Find a map marker lump (e.g. "MAP01") and return its directory index.
   Returns -1 if not found. */
int wad_find_map(const wad_t *wad, const char *map_name);

/* Find a lump by name that follows a map marker (within 11 lumps after it).
   For example, wad_find_map_lump(wad, "MAP01", "VERTEXES", &size) finds
   the VERTEXES lump belonging to MAP01.
   Returns pointer to lump data, or NULL if not found. */
const uint8_t *wad_find_map_lump(const wad_t *wad, const char *map_name,
                                  const char *lump_name, uint32_t *size_out);

/* Count the number of MAPxx markers in the WAD (MAP01..MAP32).
   Returns count (0 if none found). */
uint32_t wad_count_maps(const wad_t *wad);

/* Get the name of the map at the given zero-based index among MAPxx markers.
   Copies the name into name_out (must be at least 9 bytes).
   Returns 0 on success, -1 if index out of range. */
int wad_get_map_name(const wad_t *wad, uint32_t index, char name_out[9]);
