#include <stdint.h>
#include "multiboot.h"
#include "serial.h"
#include "fb.h"
#include "fb_console.h"
#include "fnv1a.h"

static inline void qemu_exit(uint32_t code) {
    __asm__ volatile ("outl %0, %1" : : "a"(code), "Nd"(0xF4));
}

static void serial_print_hex(uint32_t v) {
    const char hex[] = "0123456789abcdef";
    serial_print("0x");
    for (int i = 28; i >= 0; i -= 4)
        serial_putc(hex[(v >> i) & 0xf]);
}

static void serial_print_uint(uint32_t v) {
    if (v == 0) { serial_putc('0'); return; }
    char buf[10];
    int i = 0;
    while (v) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i--) serial_putc(buf[i]);
}

void kernel_main(uint32_t mb_info_addr) {
    serial_init();

    struct multiboot_info* mb = (struct multiboot_info*)mb_info_addr;

    serial_print("Kernel Booted\n");

    serial_print("mods_count=");
    serial_print_uint(mb->mods_count);
    serial_print(" mods_addr=");
    serial_print_hex(mb->mods_addr);
    serial_putc('\n');

    if (!(mb->flags & MULTIBOOT_INFO_FLAG_MODS) || mb->mods_count == 0) {
        serial_print("ERROR: no multiboot modules loaded (WAD missing)\n");
    } else {
        struct multiboot_module *mods = (struct multiboot_module *)mb->mods_addr;
        for (uint32_t i = 0; i < mb->mods_count; i++) {
            uint32_t size = mods[i].mod_end - mods[i].mod_start;
            const char *name = (const char *)(uintptr_t)mods[i].cmdline;

            serial_print("  mod[");
            serial_print_uint(i);
            serial_print("] start=");
            serial_print_hex(mods[i].mod_start);
            serial_print(" end=");
            serial_print_hex(mods[i].mod_end);
            serial_print(" size=");
            serial_print_uint(size);
            serial_print(" name=");
            serial_print(name && *name ? name : "(none)");
            serial_putc('\n');

            uint32_t checksum = fnv1a32((const void *)(uintptr_t)mods[i].mod_start, size);
            serial_print("  wad checksum=");
            serial_print_hex(checksum);
            serial_putc('\n');
        }
    }

    serial_flush();
    qemu_exit(0);

    if (!(mb->flags & MULTIBOOT_INFO_FLAG_FRAMEBUFFER)) for(;;);

    framebuffer_t fb;
    if (!fb_init_bgrx8888(&fb,
                          (uintptr_t)mb->framebuffer_addr,
                          mb->framebuffer_pitch,
                          mb->framebuffer_width,
                          mb->framebuffer_height,
                          mb->framebuffer_bpp)) {
        for(;;);
    }

    fb_console_t con;
    if (!fbcon_init(&con, &fb)) for(;;);

    fbcon_set_color(&con, 255,255,255, 0,0,0);
    fbcon_write(&con, "ExoDoom fb console online.\n");
    fbcon_write(&con, "Now printing to pixels like a proper gremlin.\n\n");
    fbcon_write(&con, "0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ\n");
    fbcon_write(&con, "abcdefghijklmnopqrstuvwxyz !@#$%^&*()[]{}\n");

    for(;;);
}
