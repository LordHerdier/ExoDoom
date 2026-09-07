#ifndef PAGE_ALLOC_H
#define PAGE_ALLOC_H

#include <stdint.h>

#include "multiboot2.h"

// Initializes the bitmap allocator using the first eligible usable memory region.
//
// NOTE: currently only the first large usable region is managed.

void page_alloc_init(const struct mb2_info* mb);

void* alloc_page(void);

void free_page(void* addr);

// Like free_page, but reports the outcome so callers (e.g. the exo_page_free
// syscall handler) can distinguish success from a bad request.  Returns 0 on
// success and non-zero (currently -1) on an unaligned/out-of-range address or a
// double free.  The non-zero value is an opaque failure flag, not an EXO_E*
// code -- the PMM stays ABI-agnostic and the caller maps failure to whatever
// error it returns to ring 3.  free_page() is a void wrapper around this.
int free_page_checked(void* addr);

#endif
