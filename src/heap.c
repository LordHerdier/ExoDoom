#include "heap.h"
#include "page_alloc.h"
#include "string.h"
#include "serial.h"
#include "stdio.h"

#include <stdint.h>

/*
 * Kernel heap (SCRUM-25) -- first-fit, segmented free-list allocator backed
 * by the PMM.  See docs/memory.md sec5b for the design writeup.
 *
 * A "segment" is one run of physically contiguous pages obtained from
 * successive alloc_page() calls.  alloc_page() scans its bitmap forward from
 * index 0, so it hands back adjacent pages as long as nothing lower has been
 * freed elsewhere -- true in practice for a heap that only ever grows before
 * SCRUM-6x page-freeing work lands, but not guaranteed, so growth checks
 * physical contiguity explicitly rather than assuming it: a non-adjacent
 * page starts a new segment instead of corrupting the block list of an
 * existing one. First-fit search walks every block in every segment; a
 * block's size never spans a segment boundary.
 *
 * A segment's own bookkeeping (segment_t) lives in the first bytes of its
 * first page, immediately followed by that page's first block header --
 * there is no separate metadata allocator to bootstrap.
 */

#define PAGE_SIZE  4096
#define HEAP_ALIGN 16
/* A split that would leave a free remainder smaller than this is skipped --
 * the whole block is handed out instead of creating a sliver too small to
 * ever satisfy a future allocation. */
#define MIN_SPLIT_PAYLOAD 16

typedef struct block {
    size_t size;          /* payload bytes, not including this header */
    int free;
    struct block *prev;   /* physically previous block, same segment, or NULL */
    struct block *next;   /* physically next block, same segment, or NULL */
} block_t;

typedef struct segment {
    uintptr_t start;       /* first byte of the segment (this header) */
    uintptr_t end;         /* one past the last byte of the segment */
    block_t  *first;       /* first block header in the segment */
    struct segment *next;
} segment_t;

static segment_t *segments      = NULL;
static segment_t *segments_tail = NULL;
/* One past the physical address of the most recently alloc_page()'d page, or
 * 0 before the first call -- lets heap_grow_one_page() tell whether the next
 * page continues the current segment or must start a new one. */
static uintptr_t heap_growth_cursor = 0;

static uintptr_t align_up(uintptr_t v, uintptr_t align) {
    return (v + align - 1) & ~(align - 1);
}

static void segment_append_block(segment_t *seg, block_t *tail_hint,
                                  uintptr_t paddr) {
    block_t *nb = (block_t *)paddr;
    nb->size = PAGE_SIZE - sizeof(block_t);
    nb->free = 1;
    nb->prev = tail_hint;
    nb->next = NULL;
    tail_hint->next = nb;
    seg->end += PAGE_SIZE;
}

/* Extend the current segment by exactly one physically-contiguous page. */
static void segment_grow(segment_t *seg, uintptr_t paddr) {
    block_t *tail = seg->first;
    while (tail->next != NULL) {
        tail = tail->next;
    }

    if (tail->free) {
        tail->size += PAGE_SIZE;
        seg->end += PAGE_SIZE;
    } else {
        segment_append_block(seg, tail, paddr);
    }
}

/* Start a brand new segment out of one page. */
static void segment_create(uintptr_t paddr) {
    segment_t *seg = (segment_t *)paddr;
    seg->start = paddr;
    seg->end   = paddr + PAGE_SIZE;
    seg->next  = NULL;

    block_t *first = (block_t *)(paddr + sizeof(segment_t));
    first->size = PAGE_SIZE - sizeof(segment_t) - sizeof(block_t);
    first->free = 1;
    first->prev = NULL;
    first->next = NULL;
    seg->first = first;

    if (segments_tail != NULL) {
        segments_tail->next = seg;
    } else {
        segments = seg;
    }
    segments_tail = seg;
}

/* Grow the heap by exactly one PMM page. Returns 0 on success, -1 if the PMM
 * is out of pages. */
static int heap_grow_one_page(void) {
    void *page = alloc_page();
    if (page == NULL) {
        serial_print("heap: out of pages\n");
        return -1;
    }

    uintptr_t paddr = (uintptr_t)page;

    if (segments_tail != NULL && paddr == heap_growth_cursor) {
        segment_grow(segments_tail, paddr);
    } else {
        segment_create(paddr);
    }

    heap_growth_cursor = paddr + PAGE_SIZE;
    return 0;
}

static block_t *find_first_fit(size_t size) {
    for (segment_t *seg = segments; seg != NULL; seg = seg->next) {
        for (block_t *b = seg->first; b != NULL; b = b->next) {
            if (b->free && b->size >= size) {
                return b;
            }
        }
    }
    return NULL;
}

/* Split `b` so its payload is exactly `size`, handing the remainder back as
 * a new free block, provided the remainder is worth keeping. */
static void split_block(block_t *b, size_t size) {
    size_t remainder = b->size - size;
    if (remainder < sizeof(block_t) + MIN_SPLIT_PAYLOAD) {
        return;
    }

    block_t *nb = (block_t *)((uintptr_t)b + sizeof(block_t) + size);
    nb->size = remainder - sizeof(block_t);
    nb->free = 1;
    nb->prev = b;
    nb->next = b->next;
    if (nb->next != NULL) {
        nb->next->prev = nb;
    }
    b->next = nb;
    b->size = size;
}

/* Merge `b` with its immediate successor if that neighbour is also free.
 * Both are always in the same segment -- block_t::next never crosses one. */
static void coalesce_with_next(block_t *b) {
    block_t *n = b->next;
    if (n == NULL || !n->free) {
        return;
    }

    b->size += sizeof(block_t) + n->size;
    b->next = n->next;
    if (b->next != NULL) {
        b->next->prev = b;
    }
}

void *heap_alloc(size_t size) {
    if (size == 0) {
        size = 1;
    }
    if (size > SIZE_MAX - HEAP_ALIGN) {
        return NULL;
    }
    size = align_up(size, HEAP_ALIGN);

    block_t *b = find_first_fit(size);
    while (b == NULL) {
        if (heap_grow_one_page() != 0) {
            return NULL;
        }
        b = find_first_fit(size);
    }

    split_block(b, size);
    b->free = 0;
    return (void *)((uintptr_t)b + sizeof(block_t));
}

void heap_free(void *ptr) {
    if (ptr == NULL) {
        return;
    }

    block_t *b = (block_t *)((uintptr_t)ptr - sizeof(block_t));
    if (b->free) {
        serial_print("heap: double free detected\n");
        return;
    }

    b->free = 1;
    coalesce_with_next(b);
    if (b->prev != NULL && b->prev->free) {
        coalesce_with_next(b->prev);
    }
}

void *heap_realloc(void *ptr, size_t size) {
    if (ptr == NULL) {
        return heap_alloc(size);
    }
    if (size == 0) {
        heap_free(ptr);
        return NULL;
    }

    if (size > SIZE_MAX - HEAP_ALIGN) {
        return NULL;
    }
    size = align_up(size, HEAP_ALIGN);
    block_t *b = (block_t *)((uintptr_t)ptr - sizeof(block_t));

    if (b->size >= size) {
        split_block(b, size);
        return ptr;
    }

    if (b->next != NULL && b->next->free &&
        b->size + sizeof(block_t) + b->next->size >= size) {
        coalesce_with_next(b);
        split_block(b, size);
        return ptr;
    }

    void *newp = heap_alloc(size);
    if (newp == NULL) {
        return NULL;
    }
    memcpy(newp, ptr, b->size < size ? b->size : size);
    heap_free(ptr);
    return newp;
}

/* ---- Accounting (SCRUM-27) ---------------------------------------------
 *
 * Walks every block of every segment; see heap.h for what the counters mean
 * and why used_bytes rather than free_bytes is the one to compare when the
 * heap may have grown in between.
 */

void heap_get_stats(heap_stats_t *out) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));

    for (segment_t *seg = segments; seg != NULL; seg = seg->next) {
        out->segments++;
        out->pages += (size_t)((seg->end - seg->start) / PAGE_SIZE);

        for (block_t *b = seg->first; b != NULL; b = b->next) {
            out->total_bytes += b->size;
            if (b->free) {
                out->free_bytes += b->size;
                out->free_blocks++;
            } else {
                out->used_bytes += b->size;
                out->used_blocks++;
            }
        }
    }
}

void heap_report(const char *label) {
    heap_stats_t s;
    heap_get_stats(&s);

    /* printf() has no length modifiers (see src/stdio.h), so %u is an
     * unsigned int -- narrow the size_t counters deliberately.  The heap
     * would have to reach 4 GiB for that to lose anything, which the PMM's
     * single managed region cannot supply. */
    printf("heap[%s]: free=%u used=%u total=%u "
           "blocks=%u/%u seg=%u pages=%u\n",
           label,
           (unsigned)s.free_bytes,
           (unsigned)s.used_bytes,
           (unsigned)s.total_bytes,
           (unsigned)s.free_blocks,
           (unsigned)s.used_blocks,
           (unsigned)s.segments,
           (unsigned)s.pages);
}
