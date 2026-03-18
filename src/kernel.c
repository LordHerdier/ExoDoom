#include <stdint.h>
#include "multiboot.h"
#include "serial.h"
#include "fb.h"
#include "fb_console.h"
#include "memory.h"

static inline void qemu_exit(uint32_t code) {
    __asm__ volatile ("outl %0, %1" : : "a"(code), "Nd"(0xF4));
}

void kernel_main(uint32_t mb_info_addr) {
    serial_init();

    struct multiboot_info* mb = (struct multiboot_info*)mb_info_addr;

    serial_print("Kernel Booted\n");

    memory_init();

    void* a = kmalloc(256);
    void* b = kmalloc(512);
    void fb_print_hex(fb_console_t* con, uint32_t num) {
    char hex[] = "0123456789ABCDEF";
    char buffer[9];
    buffer[8] = '\0';

    for (int i = 7; i >= 0; i--) {
        buffer[i] = hex[num & 0xF];
        num >>= 4;
    }

    fbcon_write(con, buffer);
}

    serial_print("Memory allocation working!\n");
    serial_print("Address A: ");
    serial_print_hex((uint32_t)a);
    serial_print("\n");

    serial_print("Address B: ");
    serial_print_hex((uint32_t)b);
    serial_print("\n");

    serial_flush();
    // qemu_exit(0); //

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
    fbcon_write(&con, "\nMemory allocation demo:\n");

fbcon_write(&con, "Address A: ");
fb_print_hex(&con, (uint32_t)a);

fbcon_write(&con, "\nAddress B: ");
fb_print_hex(&con, (uint32_t)b);

fbcon_write(&con, "\n");
    fbcon_write(&con, "Now printing to pixels like a proper gremlin.\n\n");
    fbcon_write(&con, "0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ\n");
    fbcon_write(&con, "abcdefghijklmnopqrstuvwxyz !@#$%^&*()[]{}\n");

    for(;;);

    char fb_buf[64];

fbcon_write(&con, "Memory allocation demo:\n");

fbcon_write(&con, "A: ");
serial_print_hex((uint32_t)a); // still prints to serial

fbcon_write(&con, "\nB: ");
serial_print_hex((uint32_t)b);

fbcon_write(&con, "\n");
}
