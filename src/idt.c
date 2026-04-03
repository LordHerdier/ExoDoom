#include "idt.h"
#include "io.h"

#define IDT_ENTRIES 256

struct idt_entry{
    uint16_t baseLow;
    uint16_t sel;
    uint8_t always0;
    uint8_t flags;
    uint16_t baseHigh;
} __attribute__((packed));

struct idt_ptr{
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));


static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr idtp;
static uint16_t kernel_cs;

extern void idt_load(uint32_t);
extern void default_stub(void);

void idt_init() {
    /* Read the actual code segment selector GRUB gave us rather than
       assuming 0x08 — a mismatch causes iret to GP-fault on the way out
       of any interrupt handler. */
    __asm__ volatile ("mov %%cs, %0" : "=r"(kernel_cs));

    idtp.limit = sizeof(struct idt_entry) * IDT_ENTRIES - 1;
    idtp.base = (uint32_t)&idt;

    /* Fill every entry with a safe default so any stray vector irets
       cleanly instead of cascading into a triple fault. */
    for (int i = 0; i < IDT_ENTRIES; i++)
        idt_set_gate(i, (uint32_t)default_stub);

    idt_load((uint32_t)&idtp);
}


void idt_set_gate(int n, uint32_t handler) {
    idt[n].baseLow = handler & 0xFFFF;
    idt[n].baseHigh = (handler >> 16) & 0xFFFF;
    idt[n].sel = kernel_cs;
    idt[n].always0 = 0;
    idt[n].flags = 0x8E;
}
