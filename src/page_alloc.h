#ifndef PAGE_ALLOC_H
#define PAGE_ALLOC_H

#include <stdint.h>
#include "multiboot.h"

void page_alloc_init(struct multiboot_info* mb);
void* alloc_page(void);
void free_page(void* addr);

#endif
