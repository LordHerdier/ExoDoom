#include <stdint.h>
#include "multiboot.h"
#include "io.h"
#include "serial.h"
#include "fb.h"
#include "fb_console.h"
#include "idt.h"

static inline void qemu_exit(uint32_t code) {
    outl(0xF4, code);   // now uses io.h's outl instead of inline asm
}

void kernel_main(uint32_t mb_info_addr) {
    serial_init();

    struct multiboot_info* mb = (struct multiboot_info*)mb_info_addr;

    serial_print("Kernel Booted\n");

    /* ---- Phase 1: IDT ---- */
    idt_init();

    /*
     * TODO (next steps):
     *   pic_remap(32, 40);
     *   pic_disable();
     *   idt_set_gate(32, (uint32_t)isr_stub_32, 0x08, 0x8E);
     *   pit_init(100);
     *   pic_unmask_irq(0);
     *   __asm__ volatile ("sti");
     */

    serial_print("IDT initialized, interrupts still disabled\n");
    serial_flush();

    /* Comment out or gate this for now — it kills the VM before we see anything */
    // qemu_exit(0);

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
    fbcon_write(&con, "IDT loaded. Waiting for PIC + PIT...\n");

    for(;;);
}