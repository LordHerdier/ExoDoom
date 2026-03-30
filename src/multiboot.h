#pragma once
#include <stdint.h>

struct multiboot_info {
    uint32_t flags;

    uint32_t mem_lower;
    uint32_t mem_upper;

    uint32_t boot_device;
    uint32_t cmdline;

    uint32_t mods_count;
    uint32_t mods_addr;

    uint32_t syms[4];

    uint32_t mmap_length;
    uint32_t mmap_addr;

    uint32_t drives_length;
    uint32_t drives_addr;

    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;

    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;

    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    uint8_t  color_info[6];
} __attribute__((packed));

enum {
    MULTIBOOT_INFO_FLAG_MODS = 1u << 3,
    MULTIBOOT_INFO_FLAG_MMAP = 1u << 6,
    MULTIBOOT_INFO_FLAG_FRAMEBUFFER = 1u << 12,
};

struct multiboot_mmap_entry {
    uint32_t size;
    uint64_t addr;
    uint64_t len;
    uint32_t type;
} __attribute__((packed));

struct multiboot_module {
    uint32_t mod_start;
    uint32_t mod_end;
    uint32_t cmdline;
    uint32_t pad;
};

enum {
    MULTIBOOT_MMAP_AVAILABLE    = 1,
    MULTIBOOT_MMAP_RESERVED     = 2,
    MULTIBOOT_MMAP_ACPI_RECLAIM = 3,
    MULTIBOOT_MMAP_ACPI_NVS     = 4,
    MULTIBOOT_MMAP_BADRAM       = 5,
};

enum {
    MULTIBOOT_FRAMEBUFFER_TYPE_INDEXED = 0,
    MULTIBOOT_FRAMEBUFFER_TYPE_RGB     = 1,
    MULTIBOOT_FRAMEBUFFER_TYPE_EGA     = 2,
};
