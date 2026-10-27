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
extern void error_stub(void);
extern void pf_stub(void);
extern void gpf_stub(void);

/* One stub per remaining exception vector (SCRUM-283), src/isr.s EXC_STUB. */
#define EXC_STUBS(X) \
    X(0) X(1) X(3) X(4) X(5) X(6) X(7) X(8) X(10) X(11) X(12) X(16) X(17) \
    X(18) X(19) X(20) X(21) X(28) X(29) X(30)
#define X(n) extern void exc_stub_##n(void);
EXC_STUBS(X)
#undef X

/* Vectors that push a hardware error code (SCRUM-135). default_stub's bare
 * iretq cannot handle these safely — error_stub must be installed instead.
 * Since SCRUM-283 every one of them is overridden below (8/10/11/12/17/21/29/30
 * by exc_stub_N, 13/14 by their own stubs), so error_stub is only the
 * fallback for a vector someone removes from that list. */
static const int error_code_vectors[] = { 8, 10, 11, 12, 13, 14, 17, 21, 29, 30 };

void idt_init() {
    idtp.limit = sizeof(struct idt_entry) * IDT_ENTRIES - 1;
    idtp.base  = (uint64_t)(uintptr_t)&idt;

    for (int i = 0; i < IDT_ENTRIES; i++)
        idt_set_gate(i, (uintptr_t)default_stub);

    for (unsigned i = 0; i < sizeof(error_code_vectors) / sizeof(error_code_vectors[0]); i++)
        idt_set_gate(error_code_vectors[i], (uintptr_t)error_stub);

    /* Vector 14 gets a real handler rather than the absorb-and-return stub
     * (SCRUM-17).  error_stub returns to the faulting instruction, which
     * faults again immediately -- fine as a "don't triple-fault" measure, no
     * use as a diagnostic.  Installed after the loop above so it wins. */
    idt_set_gate(14, (uintptr_t)pf_stub);

    /* Vector 13 (#GP) gets the same treatment (SCRUM-56) -- error_stub would
     * otherwise silently re-fault forever on a ring-3 IN/OUT trap. */
    idt_set_gate(13, (uintptr_t)gpf_stub);

    /* Every other CPU exception gets a real handler (SCRUM-283) rather than
     * default_stub's re-enter-the-faulting-instruction loop: a ring-3 #DE,
     * #UD, #NM, #SS, #AC, #MF, #XM ... ends that program; ring 0 halts with
     * a diagnostic. */
#define X(n) idt_set_gate(n, (uintptr_t)exc_stub_##n);
    EXC_STUBS(X)
#undef X

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
