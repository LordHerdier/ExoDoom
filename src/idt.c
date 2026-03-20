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

extern void idt_load(uint32_t);

void idtInit(){
    idtp.limit = sizeof(struct idt_entry) * IDT_ENTRIES - 1;
    idtp.base = (uint32_t)&idt;

    for (int i = 0; i < IDT_ENTRIES; i++){
        idtSetGate(i, 0);
    }

    idt_load((uint32_t)&idtp);
}


void idtSetGate(int n, uint32_t handler){
    idt[n].baseLow = handler & 0xFFFF;
    idt[n].baseHigh = (handler >> 16) & 0xFFFF;
    idt[n].sel = 0x08;
    idt[n].always0 = 0;
    idt[n].flags = 0x8E;
}