#ifndef MMAP_H
#define MMAP_H

#include <stdint.h>
#include "multiboot2.h"

#define MAX_MMAP_REGIONS 32

/* Memory type constants — re-exported from multiboot2.h for convenience */
enum {
    MULTIBOOT_MMAP_AVAILABLE    = MB2_MMAP_AVAILABLE,
    MULTIBOOT_MMAP_RESERVED     = MB2_MMAP_RESERVED,
    MULTIBOOT_MMAP_ACPI_RECLAIM = MB2_MMAP_ACPI_RECLAIM,
    MULTIBOOT_MMAP_ACPI_NVS     = MB2_MMAP_ACPI_NVS,
    MULTIBOOT_MMAP_BADRAM       = MB2_MMAP_BADRAM,
};

typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;
} mmap_region_t;

void mmap_init(const struct mb2_info *info);
const mmap_region_t *mmap_get_regions(uint32_t *count);

#endif
