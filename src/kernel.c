#include <stdint.h>
#include "multiboot.h"
#include "serial.h"
#include "fb.h"

void kernel_main(uint32_t mb_info_addr) {
    serial_init();
    serial_print("hello from kernel\n");

    struct multiboot_info* mb = (struct multiboot_info*)mb_info_addr;

    if (!(mb->flags & MULTIBOOT_INFO_FLAG_FRAMEBUFFER)) {
        serial_print("no framebuffer info (mb flags bit12 not set)\n");
        for(;;);
    }

    framebuffer_t fb;
    if (!fb_init_bgrx8888(&fb,
                          (uintptr_t)mb->framebuffer_addr,
                          mb->framebuffer_pitch,
                          mb->framebuffer_width,
                          mb->framebuffer_height,
                          mb->framebuffer_bpp)) {
        serial_print("unsupported framebuffer mode (need 32bpp)\n");
        for(;;);
    }

    fb_test_color_sanity(&fb);
    for(;;);
}
