#include <stdint.h>
#include "multiboot.h"
#include "serial.h"
#include "memory.h"
#include "mmap.h"

//IDT and Interrupt includes
#include "idt.h"
#include "pic.h"
#include "pit.h"

//IRQ0 stub from assembly
extern void irq0_stub();

static inline void qemu_exit(uint32_t code) {
    __asm__ volatile ("outl %0, %1" : : "a"(code), "Nd"(0xF4));
}

void kernel_main(uint32_t mb_info_addr) {
    serial_init();

    struct multiboot_info* mb = (struct multiboot_info*)mb_info_addr;
    serial_print("Kernel Booted\n");

    mmap_init(mb);
    memory_init();
    serial_print("Memory subsystem initialized\n");
    serial_print("Allocator base: ");
    serial_print_hex(memory_base_address());
    serial_print("\n");
    serial_flush();

    idt_init();
    pic_remap();

    //IRQ0 = Interrupt Vector 32
    idt_set_gate(32, (uint32_t)irq0_stub);
    pit_init(100);   //100hz
    serial_print("Timer Initialized\n");

    __asm__ volatile ("sti");

    serial_print("Interrupts Enabled\n");

    //Main loop
    while (1){
        __asm__ volatile ("hlt");
    }
}
