#ifndef MEMORY_H
#define MEMORY_H

#include <stddef.h>
#include <stdint.h>

void memory_init(void);
void* kmalloc(size_t size);
void  kfree(void *ptr);
uint32_t memory_base_address(void);

#endif
