#include "memory.h"
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

    uintptr_t addr = placement_address;
    placement_address = align_up(placement_address + size, 0x1000);
    return (void*)addr;
}

uint32_t memory_base_address(void) {
    return (uint32_t)placement_address;
}

/* ---- kfree: double-free detection only; bump allocator cannot reclaim ---- */

#define KFREE_TRACK_MAX 128

static void *freed_ptrs[KFREE_TRACK_MAX];
static uint32_t freed_count = 0;

void kfree(void *ptr) {
    uint32_t i;

    if (ptr == NULL) return;

    for (i = 0; i < freed_count; i++) {
        if (freed_ptrs[i] == ptr) {
            serial_print("kfree: double-free at 0x");
            serial_print_hex((uint32_t)(uintptr_t)ptr);
            serial_print("\n");
            return;
        }
    }

    if (freed_count < KFREE_TRACK_MAX)
        freed_ptrs[freed_count++] = ptr;
}
