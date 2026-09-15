#include "memory.h"
#include "page_alloc.h"
#include "heap.h"
#include "serial.h"

extern char _bss_end;

static uintptr_t placement_address = 0;
/* End of the bump region as of the moment the PMM went live -- kmalloc()
 * calls made before that point (the PMM bitmap, the owner table) are bump
 * allocations; everything after routes to the heap instead.  Recorded once
 * because placement_address itself stops moving once the heap takes over. */
static uintptr_t bump_region_end = 0;

static uintptr_t align_up(uintptr_t addr, uintptr_t align) {
    return (addr + align - 1) & ~(align - 1);
}

void memory_init(void) {
    if (placement_address != 0) return;
    placement_address = align_up((uintptr_t)&_bss_end, 0x1000);
}

void* kmalloc(size_t size) {
    if (placement_address == 0) {
        memory_init();
    }

    /*
     * page_alloc_init() calls kmalloc() twice (bitmap, owner table) to
     * bootstrap the PMM before it can serve alloc_page() itself, so kmalloc
     * can't unconditionally be the heap -- it has to keep bump-allocating
     * until the PMM it depends on is actually up, then hand off (SCRUM-25).
     * The two bump allocations become permanent kernel infrastructure and
     * are never freed; bump_region_end marks where they end so kfree/
     * krealloc can refuse a pointer into that range instead of corrupting
     * the PMM bitmap or owner table.
     */
    if (page_alloc_is_live()) {
        if (bump_region_end == 0) {
            bump_region_end = placement_address;
        }
        return heap_alloc(size);
    }

    uintptr_t addr = placement_address;
    placement_address = align_up(placement_address + size, 0x1000);
    return (void*)addr;
}

static int in_bump_region(void* ptr) {
    uintptr_t addr = (uintptr_t)ptr;
    return bump_region_end != 0 && addr >= (uintptr_t)&_bss_end &&
           addr < bump_region_end;
}

void kfree(void* ptr) {
    if (ptr == NULL) {
        return;
    }
    if (in_bump_region(ptr)) {
        serial_print("kfree: refusing to free permanent bump allocation\n");
        return;
    }
    heap_free(ptr);
}

void* krealloc(void* ptr, size_t size) {
    if (ptr != NULL && in_bump_region(ptr)) {
        serial_print("krealloc: refusing to realloc permanent bump allocation\n");
        return NULL;
    }
    return heap_realloc(ptr, size);
}

uintptr_t memory_base_address(void) {
    return placement_address;
}
