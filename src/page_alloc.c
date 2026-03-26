#include <stdint.h>
#include <stddef.h>
#include "page_alloc.h"
#include "memory.h"
#include "serial.h"

#define PAGE_SIZE 4096

static uintptr_t managed_base = 0;
static uint32_t total_pages = 0;
static uint8_t* bitmap = 0;

extern char _load_start;
extern char _bss_end;

static void bitmap_set(uint32_t index) {
    bitmap[index / 8] |= (1u << (index % 8));
}

static void bitmap_clear(uint32_t index) {
    bitmap[index / 8] &= ~(1u << (index % 8));
}

static int bitmap_test(uint32_t index) {
    return (bitmap[index / 8] >> (index % 8)) & 1u;
}

static uintptr_t align_up_uintptr(uintptr_t value, uintptr_t align) {
    return (value + align - 1) & ~(align - 1);
}

static void reserve_region(uintptr_t start, uintptr_t end) {
    start = align_up_uintptr(start, PAGE_SIZE);
    end   = align_up_uintptr(end, PAGE_SIZE);

    for (uintptr_t addr = start; addr < end; addr += PAGE_SIZE) {
        if (addr < managed_base) continue;

        uint32_t index = (addr - managed_base) / PAGE_SIZE;
        if (index < total_pages) {
            bitmap_set(index);
        }
    }
}

void page_alloc_init(struct multiboot_info* mb) {
    if (!(mb->flags & MULTIBOOT_INFO_FLAG_MMAP)) {
        serial_print("page_alloc: no mmap available\n");
        return;
    }

    if (bitmap != 0) {
        return;
    }

    uintptr_t cur = (uintptr_t)mb->mmap_addr;
    uintptr_t end = cur + mb->mmap_length;

    while (cur < end) {
        struct multiboot_mmap_entry* entry =
            (struct multiboot_mmap_entry*)cur;

        if (entry->type == MULTIBOOT_MEMORY_AVAILABLE &&
            entry->addr >= 0x100000 &&
            entry->len >= PAGE_SIZE) {

            managed_base = align_up_uintptr((uintptr_t)entry->addr, PAGE_SIZE);
            uintptr_t region_end = (uintptr_t)(entry->addr + entry->len);
            uintptr_t usable_len = region_end - managed_base;

            total_pages = (uint32_t)(usable_len / PAGE_SIZE);

            uint32_t bitmap_bytes = (total_pages + 7) / 8;
            bitmap = (uint8_t*)kmalloc(bitmap_bytes);

            for (uint32_t i = 0; i < bitmap_bytes; i++) {
                bitmap[i] = 0;
            }

            uintptr_t kernel_start = (uintptr_t)&_load_start;
            uintptr_t kernel_end   = (uintptr_t)&_bss_end;

            reserve_region(kernel_start, kernel_end);
            serial_print("page_alloc: kernel reserved\n");

            if ((mb->flags & MULTIBOOT_INFO_FLAG_MODS) && mb->mods_count > 0) {
    struct multiboot_module* mods =
        (struct multiboot_module*)mb->mods_addr;

    for (uint32_t i = 0; i < mb->mods_count; i++) {
        serial_print("module start: ");
        serial_print_hex(mods[i].mod_start);
        serial_print("\n");

        serial_print("module end: ");
        serial_print_hex(mods[i].mod_end);
        serial_print("\n");

        reserve_region((uintptr_t)mods[i].mod_start,
                       (uintptr_t)mods[i].mod_end);
    }

    serial_print("page_alloc: modules reserved\n");
}

serial_print("page_alloc: initialized\n");
return;
        cur += entry->size + sizeof(entry->size);
    }

    serial_print("page_alloc: no usable region found\n");
}

void* alloc_page(void) {
    if (bitmap == 0 || total_pages == 0) {
        serial_print("alloc_page: allocator not initialized\n");
        return 0;
    }

    for (uint32_t i = 0; i < total_pages; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            return (void*)(managed_base + ((uintptr_t)i * PAGE_SIZE));
        }
    }

    serial_print("alloc_page: out of pages\n");
    return 0;
}

void free_page(void* addr) {
    if (bitmap == 0 || total_pages == 0) {
        serial_print("free_page: allocator not initialized\n");
        return;
    }

    uintptr_t page = (uintptr_t)addr;

    if (page < managed_base || ((page - managed_base) % PAGE_SIZE) != 0) {
        serial_print("free_page: invalid page address\n");
        return;
    }

    uint32_t index = (uint32_t)((page - managed_base) / PAGE_SIZE);

    if (index >= total_pages) {
        serial_print("free_page: page out of range\n");
        return;
    }

    if (!bitmap_test(index)) {
        serial_print("free_page: double free detected\n");
        return;
    }

    bitmap_clear(index);
}

