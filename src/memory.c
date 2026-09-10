#include "memory.h"
#include "page_alloc.h"
#include "serial.h"

extern char _bss_end;

static uintptr_t placement_address = 0;

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
     * The bump pool and the PMM's pool overlap: page_alloc_init() reserves
     * everything below the bump pointer *as it stood at the time* and never
     * hears about a later kmalloc, so a bump allocation made after the PMM is
     * up can land on a page alloc_page() has already handed out.  Nothing does
     * this today (vmm.c takes its page tables from the PMM precisely so it
     * doesn't), and it is a boot-order bug rather than something to paper over
     * at runtime -- but silence here would corrupt live page tables, so say so.
     */
    if (page_alloc_is_live()) {
        serial_print("kmalloc: called after page_alloc_init -- the returned "
                     "page may already be allocated\n");
    }

    uintptr_t addr = placement_address;
    placement_address = align_up(placement_address + size, 0x1000);
    return (void*)addr;
}

uintptr_t memory_base_address(void) {
    return placement_address;
}
