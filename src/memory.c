#include "memory.h"

extern char _bss_end;

static uintptr_t placement_address = 0;

static uintptr_t align_up(uintptr_t addr, uintptr_t align) {
    return (addr + align - 1) & ~(align - 1);
}

void memory_init(void) {
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
