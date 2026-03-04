#pragma once
#include <stdint.h>

/*
 * idt.h — x86 32-bit Interrupt Descriptor Table
 *
 * Each entry (gate descriptor) is 8 bytes packed.
 * We allocate a full 256-entry table even though most slots
 * start as "not present" — the CPU needs the table to be
 * contiguous and indexed by vector number.
 */

/*
 * Gate descriptor layout (32-bit protected mode):
 *
 *  Bits 63..48  base_hi    — ISR address [31:16]
 *  Bit  47      present    — 1 = valid entry
 *  Bits 46..45  dpl        — ring allowed to INT into this gate
 *  Bit  44      zero       — always 0
 *  Bits 43..40  gate_type  — 0xE = 32-bit interrupt gate (clears IF)
 *                            0xF = 32-bit trap gate     (leaves IF)
 *  Bits 39..32  always0
 *  Bits 31..16  sel        — GDT code segment selector
 *  Bits 15..0   base_lo    — ISR address [15:0]
 */
typedef struct {
    uint16_t base_lo;   // ISR address bits 0..15
    uint16_t sel;       // GDT selector (kernel CS = 0x08)
    uint8_t  always0;   // must be zero
    uint8_t  flags;     // present | dpl | gate type
    uint16_t base_hi;   // ISR address bits 16..31
} __attribute__((packed)) idt_entry_t;

/*
 * IDTR register value — loaded by `lidt`.
 */
typedef struct {
    uint16_t limit;     // sizeof(idt) - 1
    uint32_t base;      // address of idt[0]
} __attribute__((packed)) idt_ptr_t;

/*
 * Common flag values for idt_set_gate():
 *
 *   IDT_FLAG_PRESENT    — gate is valid
 *   IDT_FLAG_RING0      — only ring 0 can use INT instruction
 *   IDT_FLAG_RING3      — ring 3 can use INT instruction (syscalls)
 *   IDT_FLAG_INT_GATE   — 32-bit interrupt gate (IF cleared on entry)
 *   IDT_FLAG_TRAP_GATE  — 32-bit trap gate     (IF unchanged)
 *
 * For a kernel-only interrupt gate: IDT_FLAG_PRESENT | IDT_FLAG_INT_GATE = 0x8E
 * For a user-callable trap gate:    IDT_FLAG_PRESENT | IDT_FLAG_RING3 | IDT_FLAG_TRAP_GATE = 0xEF
 */
#define IDT_FLAG_PRESENT    0x80
#define IDT_FLAG_RING0      0x00
#define IDT_FLAG_RING3      0x60
#define IDT_FLAG_INT_GATE   0x0E   // 32-bit interrupt gate (clears IF)
#define IDT_FLAG_TRAP_GATE  0x0F   // 32-bit trap gate (IF unchanged)

#define IDT_MAX_ENTRIES     256

/*
 * idt_init — Zero the whole table and load IDTR.
 *
 * After this, all 256 vectors are "not present".  The CPU will
 * triple-fault on any interrupt until you wire up handlers with
 * idt_set_gate().
 *
 * Call this BEFORE enabling interrupts (sti) and BEFORE PIC remap.
 */
void idt_init(void);

/*
 * idt_set_gate — Install one interrupt/trap gate.
 *
 *   vector   — interrupt vector number (0-255)
 *   isr      — address of the ISR entry point (your asm stub)
 *   sel      — GDT code segment selector, almost always 0x08
 *   flags    — typically (IDT_FLAG_PRESENT | IDT_FLAG_INT_GATE) = 0x8E
 *
 * Example:
 *   extern void isr_stub_32(void);  // defined in isr_stub.s
 *   idt_set_gate(32, (uint32_t)isr_stub_32, 0x08, 0x8E);
 */
void idt_set_gate(uint8_t vector, uint32_t isr, uint16_t sel, uint8_t flags);