#pragma once
#include <stddef.h>

/*
 * Freestanding stdlib.h (SCRUM-30).
 *
 * The subset of <stdlib.h> doomgeneric needs from the allocation, numeric
 * and sorting families.  See docs/syscall_spec.md sec2.2 for the call-count
 * audit that picked these; `calloc`, `exit`, `abort`, `atexit`, `getenv`,
 * `atof` and `system` are still Todo there.
 *
 * malloc/free/realloc are thin wrappers over the kernel heap by way of
 * kmalloc/kfree/krealloc (src/memory.c -> src/heap.c, SCRUM-25).  There is
 * no separate user-space heap yet: the LibOS runs in the kernel's own
 * address space until SCRUM-47, so both share one allocator.
 */

/* rand() returns a value in [0, RAND_MAX]. 32767 is the C-standard minimum
 * and what the reference LCG below produces. */
#define RAND_MAX 32767

void *malloc(size_t size);
void  free(void *ptr);
void *realloc(void *ptr, size_t size);

int atoi(const char *nptr);
int abs(int j);

int  rand(void);
void srand(unsigned int seed);

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *));
