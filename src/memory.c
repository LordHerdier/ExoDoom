#include "memory.h"

static size_t placement_address = 0x100000; // 1MB (safe start after kernel)

void memory_init() {
    // later you can improve this using multiboot info
}

void* kmalloc(size_t size) {
	if (placement_address & 0xFFF) {
        placement_address &= ~0xFFF;
        placement_address += 0x1000;
	}
    void* addr = (void*)placement_address;
    placement_address += size;
    return addr;
}
