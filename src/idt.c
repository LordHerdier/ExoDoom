#include "idt.h"
#include "serial.h"

/*
 * The IDT itself.  256 entries × 8 bytes = 2048 bytes.
 * BSS-zeroed on startup, so all gates start as "not present".
 */
static idt_entry_t idt[IDT_MAX_ENTRIES];
static idt_ptr_t   idtr;

/*
 * idt_set_gate — Fill in a single gate descriptor.
 *
 * We split the 32-bit ISR address across base_lo and base_hi
 * because Intel's descriptor format predates sensible decisions.
 */
void idt_set_gate(uint8_t vector, uint32_t isr, uint16_t sel, uint8_t flags) {
    idt_entry_t* e = &idt[vector];

    e->base_lo  = (uint16_t)(isr & 0xFFFF);
    e->base_hi  = (uint16_t)((isr >> 16) & 0xFFFF);
    e->sel      = sel;
    e->always0  = 0;
    e->flags    = flags;
}

/*
 * idt_init — Build the IDTR and load it.
 *
 * We don't install any gates here on purpose: the PIC, PIT,
 * and exception setup code each call idt_set_gate() for the
 * vectors they own.  This keeps the IDT module generic.
 *
 * After lidt, the CPU knows where the table lives but interrupts
 * are still disabled (IF=0) until someone executes `sti`.
 */
void idt_init(void) {
    /* Zero every entry — guarantees P bit = 0 (not present) */
    for (int i = 0; i < IDT_MAX_ENTRIES; i++) {
        idt[i].base_lo  = 0;
        idt[i].sel      = 0;
        idt[i].always0  = 0;
        idt[i].flags    = 0;   // P=0 → not present
        idt[i].base_hi  = 0;
    }

    idtr.limit = (uint16_t)(sizeof(idt_entry_t) * IDT_MAX_ENTRIES - 1);
    idtr.base  = (uint32_t)&idt[0];

    /*
     * lidt loads the IDT register from memory.
     * The operand is a pointer to the 6-byte {limit, base} struct.
     * "m" constraint lets the compiler pick the address for us.
     */
    __asm__ volatile ("lidt %0" : : "m"(idtr));

    serial_print("[idt] loaded IDT at 0x");

    /* Quick hex print of the base address for debug */
    uint32_t addr = idtr.base;
    char hex[9];
    for (int i = 7; i >= 0; i--) {
        uint8_t nib = addr & 0xF;
        hex[i] = (nib < 10) ? ('0' + nib) : ('A' + nib - 10);
        addr >>= 4;
    }
    hex[8] = '\0';
    serial_print(hex);

    serial_print(" (");
    /* Print limit in decimal — it's always 2047 for 256 entries */
    serial_print("2047");
    serial_print(" bytes)\n");
}