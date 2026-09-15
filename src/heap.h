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

/*
 * Allocator accounting (SCRUM-27).
 *
 * A snapshot taken by walking every block of every segment -- the heap keeps
 * no running counters, so this is O(blocks) and belongs in boot diagnostics
 * and tests, never in an allocation path.
 *
 * Every byte count is *payload*, excluding block headers.  That makes
 * total_bytes deliberately not invariant: splitting a block carves a new
 * header out of it and costs sizeof(block_t) of payload, and coalescing the
 * two gives the same amount back.  So a snapshot only means something next
 * to a snapshot of a comparable state -- the leak check in
 * tests/kernel/test_heap_stress_k.c compares two fully-freed heaps rather
 * than a freed heap against a busy one.
 *
 * used_bytes/used_blocks are the exception and are invariant under
 * split/coalesce, which is why they, not free_bytes, are what a leak check
 * should key on when the heap may have grown in between.
 */
typedef struct heap_stats {
    size_t total_bytes;   /* payload in every block, free and used alike  */
    size_t free_bytes;    /* payload in free blocks                       */
    size_t used_bytes;    /* payload in allocated blocks                  */
    size_t free_blocks;
    size_t used_blocks;
    size_t segments;      /* runs of physically contiguous pages          */
    size_t pages;         /* 4 KiB pages the heap holds from the PMM      */
} heap_stats_t;

/* Fill `out` with the current snapshot.  A NULL `out` is ignored. */
void heap_get_stats(heap_stats_t* out);

/* Print a one-line `label: ...` summary of the current snapshot to COM1.
 * The serial half of SCRUM-27's acceptance criterion. */
void heap_report(const char* label);

#endif
