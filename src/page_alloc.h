#ifndef PAGE_ALLOC_H
#define PAGE_ALLOC_H

#include <stdint.h>
#include "multiboot.h"

// Initializes the bitmap allocator using the first eligible usable memory region.
// NOTE: currently only the first large usable region is managed.
void page_alloc_init(struct multiboot_info* mb);
void* alloc_page(void);
void free_page(void* addr);

#endif
