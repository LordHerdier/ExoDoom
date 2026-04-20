/*
 * isr.s — 64-bit interrupt service routine stubs.
 *
 * In long mode pusha/popa do not exist.  We manually save the caller-saved
 * registers that a C handler might clobber.  Callee-saved registers
 * (RBX, RBP, R12-R15) are preserved by the C function itself per the
 * System V AMD64 ABI.
 */

.code64

/* ── IDT loader ────────────────────────────────────────────────────────── */
.global idt_load
idt_load:
    lidt (%rdi)
    ret

/* ── Default stub for unregistered vectors ─────────────────────────────── */
.global default_stub
default_stub:
    iretq

/* ── Register save / restore macros ────────────────────────────────────── */

.macro PUSH_REGS
    push %rax
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %r8
    push %r9
    push %r10
    push %r11
.endm

.macro POP_REGS
    pop  %r11
    pop  %r10
    pop  %r9
    pop  %r8
    pop  %rdi
    pop  %rsi
    pop  %rdx
    pop  %rcx
    pop  %rax
.endm

/* ── IRQ0 — PIT timer (vector 0x20) ───────────────────────────────────── */
.global irq0_stub
.extern irq0_handler

irq0_stub:
    PUSH_REGS
    call irq0_handler
    POP_REGS
    iretq

/* ── IRQ1 — PS/2 keyboard (vector 0x21) ───────────────────────────────── */
.global irq1_stub
.extern irq1_handler

irq1_stub:
    PUSH_REGS
    call irq1_handler
    POP_REGS
    iretq
