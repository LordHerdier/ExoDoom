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

void mmap_init(const struct mb2_info *info) {
    const struct mb2_tag *tag = mb2_find_tag(info, MB2_TAG_MMAP);
    if (!tag) {
        serial_print("No multiboot2 mmap tag found\n");
        region_count = 0;
        return;
    }

    const struct mb2_tag_mmap *mmap = (const struct mb2_tag_mmap *)tag;

    serial_print("Multiboot2 memory map:\n");

    const uint8_t *entries_start = (const uint8_t *)mmap + sizeof(*mmap);
    const uint8_t *entries_end   = (const uint8_t *)mmap + mmap->size;
    uint32_t entry_size = mmap->entry_size;

    region_count = 0;

    for (const uint8_t *p = entries_start;
         p < entries_end && region_count < MAX_MMAP_REGIONS;
         p += entry_size)
    {
        const struct mb2_mmap_entry *entry = (const struct mb2_mmap_entry *)p;

        regions[region_count].base   = entry->addr;
        regions[region_count].length = entry->len;
        regions[region_count].type   = entry->type;

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
    }
}

const mmap_region_t* mmap_get_regions(uint32_t* count) {
    if (count != NULL) {
        *count = region_count;
    }
    return regions;
}
