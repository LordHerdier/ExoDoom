#include "tss.h"
#include "string.h"
#include "serial.h"

/*
 * tss.c — TSS setup (SCRUM-46).  See tss.h for why RSP0 is the only field
 * that matters here.
 */

#define TSS_SELECTOR 0x30   /* must match the placeholder pair in boot.s */

struct tss64 {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

extern uint64_t gdt64[];
extern void tss_load(uint16_t selector);

/* This context's kernel stack for any CPL 3 -> CPL 0 exception (SCRUM-46).
 * Distinct from src/syscall_entry.s's syscall_stack: that one is swapped in
 * by hand for the `syscall` fast path, which the CPU never routes through
 * the TSS; this one is what the CPU itself switches to for an interrupt or
 * exception gate. Single-context, matching syscall_entry.s -- SCRUM-107's
 * per-LibOS TSS entries replace this the same way swapgs replaces the
 * single saved_user_rsp there. */
static uint8_t tss_kernel_stack[TSS_KERNEL_STACK_SIZE] __attribute__((aligned(16)));

static struct tss64 tss;

/* TSS_SELECTOR is hand-synced with the placeholder pair boot.s reserves in
 * gdt64 (see the comment there). Nothing ties the two together at compile
 * time -- boot.s is assembled separately and gdt64 is a linker symbol, so a
 * _Static_assert can't reach across that boundary. Instead, check at boot
 * that the slot we are about to patch still holds boot.s's zeroed
 * placeholder quads. If a future change reorders or inserts a descriptor
 * ahead of the TSS slot without updating TSS_SELECTOR to match, this catches
 * it here with a clear message instead of silently corrupting whatever
 * descriptor now lives at that offset. */
static void verify_tss_slot(void) {
    if (gdt64[TSS_SELECTOR / 8] != 0 || gdt64[TSS_SELECTOR / 8 + 1] != 0) {
        serial_print("tss: FATAL: gdt64[TSS_SELECTOR] is not boot.s's zeroed "
                      "placeholder -- TSS_SELECTOR no longer matches the "
                      "GDT layout in boot.s\n");
        for (;;) {
            __asm__ volatile ("cli; hlt");
        }
    }
}

/* Encode a 64-bit TSS system descriptor into the two gdt64 quads reserved
 * for it (Intel SDM 3A §7.2.3) and patch them in place. */
static void install_tss_descriptor(uint64_t base, uint32_t limit) {
    uint64_t low = 0;
    low |= (uint64_t)(limit & 0xFFFFu);
    low |= (base & 0xFFFFFFULL) << 16;
    low |= (uint64_t)0x9u << 40;              /* type: 64-bit TSS (available) */
    /* bit 44 (S) left 0: system descriptor, not code/data */
    /* bits 45-46 (DPL) left 0: only the kernel ever loads TR */
    low |= (uint64_t)1u << 47;                /* P = 1 */
    low |= (uint64_t)((limit >> 16) & 0xFu) << 48;
    /* AVL/G left 0: byte-granular limit, small enough it never needs 4K units */
    low |= ((base >> 24) & 0xFFULL) << 56;

    uint64_t high = (base >> 32) & 0xFFFFFFFFULL;

    gdt64[TSS_SELECTOR / 8]     = low;
    gdt64[TSS_SELECTOR / 8 + 1] = high;
}

void tss_init(void) {
    memset(&tss, 0, sizeof(tss));

    tss.rsp0 = (uint64_t)(uintptr_t)(tss_kernel_stack + sizeof(tss_kernel_stack));

    /* iomap_base == the TSS's own size points the I/O permission bitmap one
     * byte past the segment limit below -- every port access from a lower
     * privilege level then reads as "no bitmap" and takes #GP, which is
     * exactly the "LibOS cannot execute IN/OUT" behaviour Sprint 6 wants.
     * Free side effect of a correctly-sized TSS, not exercised by this
     * ticket's tests. */
    tss.iomap_base = sizeof(struct tss64);

    verify_tss_slot();
    install_tss_descriptor((uint64_t)(uintptr_t)&tss, sizeof(struct tss64) - 1);

    tss_load(TSS_SELECTOR);
}

uint64_t tss_rsp0(void) {
    return tss.rsp0;
}
