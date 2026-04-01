#include "fnv1a.h"

#define FNV_OFFSET_BASIS 0x811c9dc5u
#define FNV_PRIME        0x01000193u

uint32_t fnv1a32(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t hash = FNV_OFFSET_BASIS;
    for (size_t i = 0; i < len; i++) {
        hash ^= p[i];
        hash *= FNV_PRIME;
    }
    return hash;
}
