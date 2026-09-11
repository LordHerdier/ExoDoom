#ifndef HEAP_H
#define HEAP_H

#include <stddef.h>

/*
 * Kernel heap (SCRUM-25) -- a first-fit, segmented free-list allocator
 * backed by the PMM (src/page_alloc.c).  See docs/memory.md sec5b.
 *
 * Not called directly -- src/memory.c's kmalloc/kfree/krealloc route here
 * once page_alloc_is_live() is true.  heap_alloc/heap_free/heap_realloc are
 * exposed with their own names so tests can exercise the heap without going
 * through the bump/heap dispatch in kmalloc().
 */

void* heap_alloc(size_t size);
void  heap_free(void* ptr);
void* heap_realloc(void* ptr, size_t size);

#endif
