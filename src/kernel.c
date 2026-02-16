#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__linux__)
#error "You are not using a cross-compiler, you will most certainly run into trouble"
#endif

#if !defined(__i386__)
#error "This tutorial needs to be compiled with a ix86-elf compiler"
#endif

// ---------- Multiboot1 info ----------

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

    // VBE fields
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;

    // Framebuffer fields (Multiboot1 “framebuffer” extension)
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    uint8_t  color_info[6];
} __attribute__((packed));

// ---------- Minimal VGA text console ----------

enum vga_color {
    VGA_COLOR_BLACK = 0,
    VGA_COLOR_LIGHT_GREY = 7,
    VGA_COLOR_LIGHT_RED = 12,
};

static inline uint8_t vga_entry_color(enum vga_color fg, enum vga_color bg) {
    return fg | bg << 4;
}

static inline uint16_t vga_entry(unsigned char uc, uint8_t color) {
    return (uint16_t)uc | (uint16_t)color << 8;
}

#define VGA_WIDTH   80
#define VGA_HEIGHT  25
#define VGA_MEMORY  0xB8000

static size_t terminal_row;
static size_t terminal_column;
static uint8_t terminal_color;
static uint16_t* const terminal_buffer = (uint16_t*)VGA_MEMORY;

static void terminal_initialize(void) {
    terminal_row = 0;
    terminal_column = 0;
    terminal_color = vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);

    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            const size_t index = y * VGA_WIDTH + x;
            terminal_buffer[index] = vga_entry(' ', terminal_color);
        }
    }
}

static void terminal_putchar(char c) {
    if (c == '\n') {
        terminal_row++;
        terminal_column = 0;
        return;
    }

    const size_t index = terminal_row * VGA_WIDTH + terminal_column;
    terminal_buffer[index] = vga_entry(c, terminal_color);

    if (++terminal_column == VGA_WIDTH) {
        terminal_column = 0;
        if (++terminal_row == VGA_HEIGHT)
            terminal_row = 0;
    }
}

static void terminal_writestring(const char* s) {
    while (*s) {
        terminal_putchar(*s++);
    }
}

static void print_hex32(uint32_t v) {
    const char* hex = "0123456789ABCDEF";
    for (int i = 7; i >= 0; --i) {
        uint8_t nib = (v >> (i * 4)) & 0xF;
        terminal_putchar(hex[nib]);
    }
}

static void print_hex64(uint64_t v) {
    print_hex32((uint32_t)(v >> 32));
    terminal_putchar('_');
    print_hex32((uint32_t)(v & 0xFFFFFFFFu));
}

static void print_dec(uint32_t v) {
    char buf[16];
    int i = 0;
    if (v == 0) {
        terminal_putchar('0');
        return;
    }
    while (v > 0 && i < 15) {
        buf[i++] = '0' + (v % 10);
        v /= 10;
    }
    while (i--) {
        terminal_putchar(buf[i]);
    }
}

// ---------- kernel_main ----------

void kernel_main(uint32_t mb_info_addr)
{
    struct multiboot_info* mb = (struct multiboot_info*)mb_info_addr;

    terminal_initialize();
    terminal_writestring("ExoDoom framebuffer debug\n\n");

    terminal_writestring("flags = 0x");
    print_hex32(mb->flags);
    terminal_putchar('\n');

    if (!(mb->flags & (1u << 12))) {
        terminal_writestring("bit 12 NOT set (no framebuffer info)\n");
        for(;;);
    }

    terminal_writestring("bit 12 set (framebuffer info present)\n\n");

    terminal_writestring("framebuffer_addr = 0x");
    print_hex64(mb->framebuffer_addr);
    terminal_putchar('\n');

    terminal_writestring("pitch            = ");
    print_dec(mb->framebuffer_pitch);
    terminal_putchar('\n');

    terminal_writestring("width            = ");
    print_dec(mb->framebuffer_width);
    terminal_putchar('\n');

    terminal_writestring("height           = ");
    print_dec(mb->framebuffer_height);
    terminal_putchar('\n');

    terminal_writestring("bpp              = ");
    print_dec(mb->framebuffer_bpp);
    terminal_putchar('\n');

    terminal_writestring("type             = ");
    print_dec(mb->framebuffer_type);
    terminal_putchar('\n');

    for(;;);
}
