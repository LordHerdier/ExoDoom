#pragma once
#include <stdint.h>
#include "fb.h"
#include "wad.h"

/* Render a top-down 2D automap of the given map (e.g. "MAP01") from the WAD.
 *
 * Draws LINEDEFS as colored lines:
 *   - One-sided (walls):   red
 *   - Two-sided (passable): dark gray
 *   - Secret lines:        magenta
 *
 * Draws THINGS as small dots:
 *   - Player start (type 1): green
 *   - Monsters:              red
 *   - Items/pickups:         yellow
 *
 * The map is scaled and centered to fit within the given screen region
 * (margin pixels from each edge).
 *
 * Returns 0 on success, -1 if map data could not be found/parsed.
 */
int automap_render(framebuffer_t *fb, const wad_t *wad, const char *map_name,
                   uint32_t margin);
