#include "idt.h"
#include "io.h"

#define IDT_ENTRIES 256

struct idt_entry {
    uint16_t offset_low;    /* bits  0-15 of handler address */
    uint16_t selector;      /* code segment selector         */
    uint8_t  ist;           /* IST index (0 = legacy stack)  */
    uint8_t  type_attr;     /* type and attributes           */
    uint16_t offset_mid;    /* bits 16-31                    */
    uint32_t offset_high;   /* bits 32-63                    */
    uint32_t reserved;      /* must be 0                     */
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr idtp;

extern void idt_load(struct idt_ptr *);
extern void default_stub(void);

void idt_init() {
    idtp.limit = sizeof(struct idt_entry) * IDT_ENTRIES - 1;
    idtp.base  = (uint64_t)(uintptr_t)&idt;

    for (int i = 0; i < IDT_ENTRIES; i++)
        idt_set_gate(i, (uintptr_t)default_stub);

    idt_load(&idtp);
}

void idt_set_gate(int n, uintptr_t handler) {
    idt[n].offset_low  = (uint16_t)(handler & 0xFFFF);
    idt[n].offset_mid  = (uint16_t)((handler >> 16) & 0xFFFF);
    idt[n].offset_high = (uint32_t)((handler >> 32) & 0xFFFFFFFF);
    idt[n].selector    = 0x08;  /* kernel code segment from our GDT */
    idt[n].ist         = 0;
    idt[n].type_attr   = 0x8E;  /* present, DPL=0, interrupt gate   */
    idt[n].reserved    = 0;
}
