#include <stdint.h>
#include <stddef.h>
#include "mmap.h"
#include "serial.h"

static mmap_region_t regions[MAX_MMAP_REGIONS];
static uint32_t region_count = 0;

static const char* region_type_name(uint32_t type) {
    switch (type) {
        case MULTIBOOT_MMAP_AVAILABLE:    return "usable";
        case MULTIBOOT_MMAP_RESERVED:     return "reserved";
        case MULTIBOOT_MMAP_ACPI_RECLAIM: return "acpi reclaimable";
        case MULTIBOOT_MMAP_ACPI_NVS:     return "acpi nvs";
        case MULTIBOOT_MMAP_BADRAM:       return "bad ram";
        default:                          return "unknown";
    }
}

void mmap_init(struct multiboot_info* mb) {
    if (!(mb->flags & MULTIBOOT_INFO_FLAG_MMAP)) {
        serial_print("No multiboot mmap available\n");
        region_count = 0;
        return;
    }

    serial_print("Multiboot memory map:\n");

    uintptr_t cur = (uintptr_t)mb->mmap_addr;
    uintptr_t end = cur + mb->mmap_length;

    region_count = 0;

    while (cur < end && region_count < MAX_MMAP_REGIONS) {
        struct multiboot_mmap_entry* entry =
            (struct multiboot_mmap_entry*)cur;

        regions[region_count].base = entry->addr;
        regions[region_count].length = entry->len;
        regions[region_count].type = entry->type;

        serial_print(" base=0x");
        serial_print_hex64(entry->addr);

        serial_print(" len=0x");
        serial_print_hex64(entry->len);

        serial_print(" type=");
        serial_print_dec(entry->type);

        serial_print(" ");
        serial_print(region_type_name(entry->type));
        serial_print("\n");

        region_count++;
        cur += entry->size + sizeof(entry->size);
    }
}

const mmap_region_t* mmap_get_regions(uint32_t* count) {
    if (count != NULL) {
        *count = region_count;
    }
    return regions;
}
