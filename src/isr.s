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

/*
 * ── ALIGN_CALL_STACK / RESTORE_CALL_STACK ──────────────────────────────────
 *
 * The SysV AMD64 ABI requires %rsp % 16 == 0 immediately before a `call`.
 * The CPU does not guarantee any particular alignment of %rsp at interrupt
 * entry, and the byte-count of PUSH_REGS (9 * 8 = 72 bytes, or 72 + 8 = 80
 * with an error code) is not a reliable way to *derive* alignment — it just
 * happens to work out for some vectors and not others. Rather than count
 * bytes, save the exact pre-alignment %rsp on the stack, mask %rsp down to
 * 16 bytes, and restore the exact saved value afterwards so POP_REGS/iretq
 * see the untouched interrupt frame.
 *
 * ALIGN_CALL_STACK:
 *   push %rbp             // callee-saved; used purely as a stable anchor
 *   mov  %rsp, %rbp       // rbp = rsp *before* masking (post-push)
 *   and  $-16, %rsp       // mask rsp to 16-byte alignment for the call
 *
 * RESTORE_CALL_STACK:
 *   mov  %rbp, %rsp       // undo the masking — back to the pushed value
 *   pop  %rbp             // restore caller's rbp, rsp is back to pre-ALIGN
 *
 * Net effect on %rsp is zero (one push, one pop), and %rbp — callee-saved,
 * so the C handler must preserve it, which every ABI-compliant function
 * does — is restored to its original value before iretq.
 */
.macro ALIGN_CALL_STACK
    push %rbp
    mov  %rsp, %rbp
    and  $-16, %rsp
.endm

.macro RESTORE_CALL_STACK
    mov  %rbp, %rsp
    pop  %rbp
.endm

/* ── IRQ0 — PIT timer (vector 0x20) ───────────────────────────────────── */
.global irq0_stub
.extern irq0_handler

irq0_stub:
    PUSH_REGS
    ALIGN_CALL_STACK
    call irq0_handler
    RESTORE_CALL_STACK
    POP_REGS
    iretq

/* ── IRQ1 — PS/2 keyboard (vector 0x21) ───────────────────────────────── */
.global irq1_stub
.extern irq1_handler

irq1_stub:
    PUSH_REGS
    ALIGN_CALL_STACK
    call irq1_handler
    RESTORE_CALL_STACK
    POP_REGS
    iretq

/*
 * ── error_stub — absorbs the 10 error-code exception vectors ───────────
 * (SCRUM-135) Installed on vectors 8, 10, 11, 12, 13, 14, 17, 21, 29, 30.
 * Those vectors push an 8-byte error code below the normal interrupt frame
 * (RIP/CS/RFLAGS/RSP/SS). default_stub's bare `iretq` would misread that
 * error code as the return RIP and misalign the stack, triple-faulting the
 * machine. There is no dedicated C fault handler yet (page-fault handling
 * is Sprint 2 work), so this stub just needs to safely discard the error
 * code and return, leaving the machine in a coherent (if diagnostic-free)
 * state instead of triple-faulting.
 *
 * Stack layout at entry (top of stack downward):
 *   [error code]  <- pushed by CPU
 *   [RIP][CS][RFLAGS][RSP][SS]  <- pushed by CPU (the iretq frame)
 *
 * After PUSH_REGS, the error code sits 72 bytes below the top of our saved
 * registers (9 * 8 bytes). POP_REGS restores those 9 registers, leaving the
 * error code back on top of the stack, immediately below the iretq frame —
 * `add $8, %rsp` discards exactly the error code and nothing else.
 */
.global error_stub
error_stub:
    PUSH_REGS
    POP_REGS
    add  $8, %rsp        // discard the CPU-pushed error code
    iretq
