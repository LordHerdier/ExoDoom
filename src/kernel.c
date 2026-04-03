#include <stdint.h>
#include "multiboot.h"
#include "serial.h"
#include "memory.h"
#include "mmap.h"
#include "page_alloc.h"

static inline void qemu_exit(uint32_t code) {
    __asm__ volatile ("outl %0, %1" : : "a"(code), "Nd"(0xF4));
}

void kernel_main(uint32_t mb_info_addr) {
    serial_init();

    struct multiboot_info* mb = (struct multiboot_info*)mb_info_addr;

    serial_print("Kernel Booted\n");

    mmap_init(mb);

    memory_init();

#ifdef TESTING
    serial_flush();
    qemu_exit((uint32_t)run_tests());
#else
    serial_print("Memory subsystem initialized\n");
    serial_print("Allocator base: ");
    serial_print_hex(memory_base_address());
    serial_print("\n");	

    page_alloc_init(mb);

#ifdef DEBUG
    void* p1 = alloc_page();
    void* p2 = alloc_page();
    // p2 is intentionally not freed here because this is a short-lived smoke test.

    serial_print("Allocated page 1: ");
    serial_print_hex((uint32_t)p1);
    serial_print("\n");

    serial_print("Allocated page 2: ");
    serial_print_hex((uint32_t)p2);
    serial_print("\n");

    free_page(p1);
    serial_print("Freed page 1\n");

    free_page(p1);
#endif 

    serial_flush();

    idt_init();
    pic_remap();

    //IRQ0 vector 32
    idt_set_gate(32, (uint32_t)irq0_stub);
    pit_init(1000);   //1000hz
    serial_print("Timer Initialized\n");

    __asm__ volatile ("sti");

    serial_print("Interrupts Enabled\n");

    //Sleep test case
    serial_print("Sleeping for 1 second...\n");
    kernel_sleep_ms(1000);
    serial_print("Done sleeping!\n");

    //Print monotonic ms counter for 5 seconds then exit
    uint32_t prints = 0;
    while (prints < 5) {
        if (pit_take_print_pending()) {
            serial_print("ms: ");
            serial_print_u32(kernel_get_ticks_ms());
            serial_print("\n");
            prints++;
        }
        __asm__ volatile ("hlt");
    }

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
#endif
}
