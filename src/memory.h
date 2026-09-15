#ifndef MEMORY_H
#define MEMORY_H

#include <stddef.h>
#include <stdint.h>

void memory_init(void);
void* kmalloc(size_t size);
uintptr_t memory_base_address(void);

/* kfree/krealloc (SCRUM-25) only ever apply to a kmalloc() return made after
 * the PMM went live -- kmalloc()'s pre-PMM bump allocations (the PMM bitmap
 * and owner table) are permanent kernel infrastructure and are never freed;
 * see the implementation note in memory.c. */
void  kfree(void* ptr);
void* krealloc(void* ptr, size_t size);

#endif
