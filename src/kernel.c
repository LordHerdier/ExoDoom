#include <stdint.h>
#include "multiboot.h"
#include "serial.h"
#include "fb.h"
#include "fb_console.h"

static inline void qemu_exit(uint32_t code) {
    __asm__ volatile ("outl %0, %1" : : "a"(code), "Nd"(0xF4));
}

void kernel_main(uint32_t mb_info_addr) {
    serial_init();

    struct multiboot_info* mb = (struct multiboot_info*)mb_info_addr;

    serial_print("Kernel Booted\n");

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

    qemu_exit(0);

    for(;;);
}
