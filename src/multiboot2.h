#pragma once
#include <stdint.h>

/* ── Multiboot 2 boot information ──────────────────────────────────────── */

struct mb2_info {
    uint32_t total_size;
    uint32_t reserved;
    /* tags follow immediately */
};

struct mb2_tag {
    uint32_t type;
    uint32_t size;
};

/* Tag type constants */
enum {
    MB2_TAG_END         = 0,
    MB2_TAG_CMDLINE     = 1,
    MB2_TAG_MODULE      = 3,
    MB2_TAG_BASIC_MEM   = 4,
    MB2_TAG_MMAP        = 6,
    MB2_TAG_FRAMEBUFFER = 8,
};

/* ── Memory map (type 6) ────────────────────────────────────────────────── */

struct mb2_mmap_entry {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t reserved;
};

struct mb2_tag_mmap {
    uint32_t type;          /* MB2_TAG_MMAP */
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    /* struct mb2_mmap_entry entries[]; */
};

/* Memory types (same values as Multiboot 1) */
enum {
    MB2_MMAP_AVAILABLE    = 1,
    MB2_MMAP_RESERVED     = 2,
    MB2_MMAP_ACPI_RECLAIM = 3,
    MB2_MMAP_ACPI_NVS     = 4,
    MB2_MMAP_BADRAM       = 5,
};

/* ── Framebuffer (type 8) ───────────────────────────────────────────────── */

struct mb2_tag_framebuffer {
    uint32_t type;          /* MB2_TAG_FRAMEBUFFER */
    uint32_t size;
    uint64_t addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t  bpp;
    uint8_t  fb_type;
    uint8_t  reserved;
    /* color info follows for indexed modes */
} __attribute__((packed));

/* ── Module (type 3) ────────────────────────────────────────────────────── */

struct mb2_tag_module {
    uint32_t type;          /* MB2_TAG_MODULE */
    uint32_t size;
    uint32_t mod_start;
    uint32_t mod_end;
    /* null-terminated command line string follows */
};

/* ── Helper: iterate tags ───────────────────────────────────────────────── */

static inline const struct mb2_tag *mb2_first_tag(const struct mb2_info *info) {
    return (const struct mb2_tag *)((uintptr_t)info + 8);
}

static inline const struct mb2_tag *mb2_next_tag(const struct mb2_tag *tag) {
    /* Tags are 8-byte aligned; size includes the type+size header */
    uintptr_t next = (uintptr_t)tag + ((tag->size + 7) & ~7u);
    return (const struct mb2_tag *)next;
}

static inline const struct mb2_tag *mb2_find_tag(const struct mb2_info *info, uint32_t type) {
    const struct mb2_tag *tag = mb2_first_tag(info);
    const uintptr_t end = (uintptr_t)info + info->total_size;
    while ((uintptr_t)tag < end && tag->type != MB2_TAG_END) {
        if (tag->type == type)
            return tag;
        tag = mb2_next_tag(tag);
    }
    return (void *)0;
}
