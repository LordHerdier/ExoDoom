#ifndef MMAP_H
#define MMAP_H

#include <stdint.h>
#include "multiboot.h"

#define MAX_MMAP_REGIONS 32

typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;
} mmap_region_t;

void mmap_init(struct multiboot_info* mb);
const mmap_region_t* mmap_get_regions(uint32_t* count);

#endif
