#ifndef MEMORY_H
#define MEMORY_H

#include <stddef.h>
#include <stdint.h>

void memory_init(void);
void* kmalloc(size_t size);
uintptr_t memory_base_address(void);

#endif
