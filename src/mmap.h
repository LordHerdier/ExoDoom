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

/* The multiboot info pointer mmap_init() was called with, for callers (tests
 * included) that need to walk tags mmap_region_t does not expose. NULL
 * before mmap_init() has run. */
const struct mb2_info *mmap_get_info(void);

/* Physical [*start, *end) of the first GRUB-loaded module (the WAD, in this
 * kernel's boot setup -- see grub.cfg). Returns 0 and writes *start / *end on a
 * match, -1 if no MB2_TAG_MODULE tag is present or mmap_init() has not run.
 * The single source of truth for "where is the WAD" — page_alloc_init() used
 * to walk the tag list itself to answer this (SCRUM-16).
 *
 * There is more than one module now (src/grub.cfg loads the boot animation
 * too), so "the first" is load-bearing: the WAD's `module2` line must stay
 * first in grub.cfg, since GRUB emits module tags in config order and every
 * caller of this function means the WAD. Anything else is looked up by name
 * with mmap_find_module_named() below. */
int mmap_find_module(uint64_t *start, uint64_t *end);

/* Physical [*start, *end) of the GRUB-loaded module whose command line is
 * exactly `name` -- for `module2 /boot/tenor.gif tenor.gif`, that is
 * "tenor.gif". Returns 0 on a match, -1 otherwise (no such module, or
 * mmap_init() has not run), which is the ordinary answer on a `-kernel` boot:
 * QEMU loads no modules at all there, so a caller must treat a miss as "this
 * boot has no such asset" rather than as an error. */
int mmap_find_module_named(const char *name, uint64_t *start, uint64_t *end);

#endif
