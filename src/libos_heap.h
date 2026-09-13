#ifndef LIBOS_HEAP_H
#define LIBOS_HEAP_H

#include <stddef.h>

/*
 * libos_heap — LibOS-side heap allocator (SCRUM-38). See docs/memory.md §8.
 *
 * Same first-fit, segmented free-list design as src/heap.c (the kernel
 * heap, docs/memory.md §6b), with one substitution: pages come from
 * libos_page_alloc() (SCRUM-37, itself backed by the memory syscalls)
 * instead of alloc_page() (direct PMM access). Not a shared engine with
 * heap.c — that file is hard-wired to alloc_page(), and turning it into a
 * page-source-parameterized abstraction is more refactor than either
 * ticket asks for. This is a new, parallel file with the same proven
 * block/segment split-coalesce logic.
 *
 * The other substitution: heap.c's segment-growth check is *physical*-
 * address contiguity (successive alloc_page() calls hand back adjacent
 * physical pages, in practice). This heap's is *virtual*-address
 * contiguity (successive libos_page_alloc() calls hand back adjacent
 * virtual addresses, in practice, since nothing here or elsewhere calls
 * libos_page_free() in steady state — this heap never shrinks, same as
 * heap.c). Both checks exist for the same reason: the assumption is not
 * guaranteed, so growth verifies it rather than trusting it — a broken
 * assumption starts a new segment instead of corrupting an existing one's
 * block list, the same self-limiting fragmentation tradeoff heap.c already
 * accepts (see its own "stranded segment" note in docs/memory.md §6b).
 *
 * Not in scope: re-pointing src/stdlib.c's malloc/free/realloc here — see
 * src/libos_page_alloc.h's note on why that stays out of both SCRUM-37 and
 * SCRUM-38.
 */

void *libos_heap_alloc(size_t size);
void  libos_heap_free(void *ptr);
void *libos_heap_realloc(void *ptr, size_t size);

/* Same shape as heap_stats_t (src/heap.h) — see that header for what each
 * field means and why used_bytes/used_blocks, not free_bytes, is what a
 * leak check should key on when the heap may have grown in between. */
typedef struct libos_heap_stats {
    size_t total_bytes;   /* payload in every block, free and used alike */
    size_t free_bytes;    /* payload in free blocks                      */
    size_t used_bytes;    /* payload in allocated blocks                 */
    size_t free_blocks;
    size_t used_blocks;
    size_t segments;      /* runs of virtually contiguous pages          */
    size_t pages;         /* 4 KiB pages the heap holds from libos_page_alloc() */
} libos_heap_stats_t;

/* Fill `out` with the current snapshot. A NULL `out` is ignored. */
void libos_heap_get_stats(libos_heap_stats_t *out);

/* Print a one-line `label: ...` summary of the current snapshot to COM1. */
void libos_heap_report(const char *label);

#endif /* LIBOS_HEAP_H */
