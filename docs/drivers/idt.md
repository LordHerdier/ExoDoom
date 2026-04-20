# Driver: IDT, ISR, and Interrupt Handling

**Files:** `src/idt.c`, `src/idt.h`, `src/isr.s`, `src/io.h` **Status:** ✅
Partial — basic IDT wired, dedicated handlers in progress **Last updated:** 2
Apr 2026

---

## Table of Contents

1. [Purpose](#1-purpose)
2. [x86 interrupt mechanics](#2-x86-interrupt-mechanics)
3. [IDT structure](#3-idt-structure)
4. [Initialisation](#4-initialisation)
5. [Assembly stubs](#5-assembly-stubs)
6. [Adding a new IRQ handler](#6-adding-a-new-irq-handler)
7. [Exception vectors and error codes](#7-exception-vectors-and-error-codes)
8. [API reference](#8-api-reference)
9. [Planned handlers](#9-planned-handlers)
10. [Design decisions and gotchas](#10-design-decisions-and-gotchas)

---

## 1. Purpose

The Interrupt Descriptor Table (IDT) is the kernel's central dispatch table for
all CPU exceptions and hardware IRQs. Every interrupt or exception the CPU
raises is looked up in the IDT by its vector number, and control is transferred
to the corresponding handler. Without a correctly initialised IDT, any interrupt
— including the PIT timer tick — will triple-fault the machine.

---

## 2. x86 interrupt mechanics

When an interrupt or exception fires on x86 (i386), the CPU:

1. Finishes the current instruction (for most exceptions; some are precise).
2. If a privilege level change is required (ring 3 → ring 0), the CPU loads the
   kernel stack from the TSS and switches stacks.
3. Pushes `EFLAGS`, `CS`, and `EIP` onto the (kernel) stack — the return
   address.
4. For certain exceptions: pushes an additional **error code** (see §7).
5. Looks up the vector number in the IDT to find the handler address.
6. Clears `IF` (disables further interrupts) for interrupt gates.
7. Transfers control to the handler.

The handler returns via `iret`, which pops `EIP`, `CS`, and `EFLAGS` in one
atomic operation, restoring the interrupted context. If an error code was
pushed, the handler **must pop it before `iret`** — otherwise `iret` reads the
error code as the return `EIP` and the stack is misaligned, causing a triple
fault.

---

## 3. IDT structure

The IDT is an array of 256 8-byte gate descriptors. ExoDoom uses 32-bit
interrupt gates exclusively (`flags = 0x8E`):

```c
struct idt_entry {
    uint16_t baseLow;   // handler address bits 0–15
    uint16_t sel;       // code segment selector (from %cs)
    uint8_t  always0;   // must be 0
    uint8_t  flags;     // gate type + DPL + present bit
    uint16_t baseHigh;  // handler address bits 16–31
} __attribute__((packed));
```

**`flags = 0x8E` breakdown:**

```
 7   6   5   4   3   2   1   0
┌───┬───┬───┬───┬───┬───┬───┬───┐
│ P │DPL│DPL│ S │ D │ 1 │ 1 │ 0 │
└───┴───┴───┴───┴───┴───┴───┴───┘
  1   0   0   0   1   1   1   0
  │   └───┘   │   └───────────┘
present DPL=0  sys  32-bit int gate
```

- **P=1**: gate is present
- **DPL=00**: descriptor privilege level 0 (ring 0 only — LibOS cannot invoke
  these via `int N` directly; the syscall gate at `0x80` will use DPL=3)
- **S=0**: system descriptor
- **Type=1110**: 32-bit interrupt gate (clears `IF` on entry)

The IDT pointer struct passed to `lidt`:

```c
struct idt_ptr {
    uint16_t limit;   // sizeof(idt) - 1
    uint32_t base;    // physical address of idt array
} __attribute__((packed));
```

---

## 4. Initialisation

`idt_init()` is called from `kernel_main` after `memory_init()` and before
`pic_remap()`:

```c
void idt_init(void) {
    // Read the actual CS selector GRUB gave us
    __asm__ volatile ("mov %%cs, %0" : "=r"(kernel_cs));

    idtp.limit = sizeof(struct idt_entry) * IDT_ENTRIES - 1;
    idtp.base  = (uint32_t)&idt;

    // Fill all 256 entries with default_stub
    for (int i = 0; i < IDT_ENTRIES; i++)
        idt_set_gate(i, (uint32_t)default_stub);

    idt_load((uint32_t)&idtp);
}
```

**Runtime `%cs` read.** The code segment selector is read from `%cs` at runtime
rather than assuming `0x08`. GRUB may use a non-standard GDT and the selector
value it chose might differ. If `sel` in the IDT entry doesn't match the actual
CS, every `iret` will general-protection-fault, which recurses until a triple
fault. Reading `%cs` directly eliminates the assumption.

**Fill before load.** All 256 entries are initialised with `default_stub` before
`lidt` is called. This ensures there is no window where a vector is present in
the CPU's view but points to uninitialised memory.

---

## 5. Assembly stubs

All handler stubs live in `src/isr.s`.

### `idt_load`

```asm
.global idt_load
idt_load:
    mov 4(%esp), %eax   // first argument: IDT pointer struct address
    lidt (%eax)
    ret
```

Called from C as `idt_load((uint32_t)&idtp)`. The `lidt` instruction accepts a
6-byte memory operand (the `idt_ptr` struct) and loads the IDT register.

### `default_stub`

```asm
.global default_stub
default_stub:
    iret
```

Installed on all 256 IDT entries during `idt_init`. Silently returns from any
unhandled interrupt or exception — correct for non-error-code vectors, **fatal
for error-code vectors** (see §7 and §10).

### `irq0_stub`

```asm
.global irq0_stub
.extern irq0_handler

irq0_stub:
    pusha
    call irq0_handler
    popa
    iret
```

Installed on vector 32 (IRQ0 / PIT timer). `pusha` saves all general-purpose
registers (`EAX`, `ECX`, `EDX`, `EBX`, `ESP`, `EBP`, `ESI`, `EDI`) as a single
instruction. After `irq0_handler` returns (having sent EOI), `popa` restores
them and `iret` returns to the interrupted context.

The stub does **not** save/restore segment registers (`DS`, `ES`, `FS`, `GS`).
This is safe while the kernel runs in a single flat segment (ring 0 only). Once
the LibOS runs in ring 3, the syscall entry stub will need to save and restore
segment registers on the stack switch.

---

## 6. Adding a new IRQ handler

To add a handler for a new IRQ (e.g., IRQ1 for the keyboard at vector 33):

**Step 1 — Write the assembly stub in `src/isr.s`:**

```asm
.global irq1_stub
.extern irq1_handler

irq1_stub:
    pusha
    call irq1_handler
    popa
    iret
```

**Step 2 — Declare the extern in the relevant C file or header:**

```c
extern void irq1_stub(void);
```

**Step 3 — Implement the C handler:**

```c
void irq1_handler(void) {
    uint8_t scancode = inb(0x60);
    // ... process scancode ...
    pic_send_EOI(1);
}
```

**Step 4 — Register the gate and unmask the IRQ, after `idt_init()` and
`pic_remap()`:**

```c
idt_set_gate(33, (uint32_t)irq1_stub);

// Unmask IRQ1 in the PIC
uint8_t mask = inb(0x21);
outb(0x21, mask & ~(1 << 1));
```

The order matters: register the gate before unmasking the IRQ. If the IRQ fires
before the gate is installed, `default_stub` will handle it silently — but it's
cleaner to eliminate the race.

---

## 7. Exception vectors and error codes

x86 reserves vectors 0–31 for CPU exceptions. Of these, a subset pushes a 32-bit
error code onto the stack before transferring to the handler. `default_stub`'s
bare `iret` will misread this error code as the return `EIP`, misalign the
stack, and triple-fault.

**Vectors that push an error code:**

| Vector | Exception                        | Error code pushed? |
| ------ | -------------------------------- | ------------------ |
| 0      | Divide Error                     | No                 |
| 1      | Debug                            | No                 |
| 2      | NMI                              | No                 |
| 3      | Breakpoint                       | No                 |
| 4      | Overflow                         | No                 |
| 5      | Bound Range Exceeded             | No                 |
| 6      | Invalid Opcode                   | No                 |
| 7      | Device Not Available             | No                 |
| **8**  | **Double Fault**                 | **Yes**            |
| 9      | Coprocessor Segment Overrun      | No                 |
| **10** | **Invalid TSS**                  | **Yes**            |
| **11** | **Segment Not Present**          | **Yes**            |
| **12** | **Stack-Segment Fault**          | **Yes**            |
| **13** | **General Protection Fault**     | **Yes**            |
| **14** | **Page Fault**                   | **Yes**            |
| 15     | Reserved                         | —                  |
| 16     | x87 FPU Error                    | No                 |
| **17** | **Alignment Check**              | **Yes**            |
| 18     | Machine Check                    | No                 |
| 19     | SIMD FP Exception                | No                 |
| 20     | Virtualisation Exception         | No                 |
| **21** | **Control Protection Exception** | **Yes**            |
| **29** | **VMM Communication Exception**  | **Yes**            |
| **30** | **Security Exception**           | **Yes**            |

> ⚠️ **Active blocker (SCRUM-135, High):** `default_stub` is currently installed
> on all of these vectors. Any one of them firing — most likely vector 13 (GPF)
> or vector 14 (page fault) — will immediately triple-fault the machine. A
> dedicated `error_stub` must be installed on vectors 8, 10, 11, 12, 13, 14, 17,
> 21, 29, and 30 before any paging work begins in Sprint 2.

**The `error_stub` fix:**

```asm
.global error_stub
error_stub:
    add $4, %esp    // discard the error code pushed by the CPU
    iret
```

This is the minimal fix. More useful would be a handler that prints diagnostic
information before halting, particularly for vectors 13 and 14 which are
actionable:

```asm
.global gpf_stub
.extern gpf_handler

gpf_stub:
    // error code is on top of stack
    pusha
    push %esp           // pass pointer to error code on stack
    call gpf_handler
    add $4, %esp
    popa
    add $4, %esp        // discard error code
    iret

// In C:
void gpf_handler(uint32_t *stack) {
    uint32_t error_code = *(stack + 8); // above pusha's 8 registers
    serial_print("GPF! error_code=0x");
    serial_print_hex(error_code);
    serial_print("\n");
    for (;;) __asm__ volatile ("hlt");
}
```

---

## 8. API reference

```c
void idt_init(void);
```

Zero-fill the IDT, install `default_stub` on all 256 vectors, and call `lidt`.
Must be called before `pic_remap()` and before `sti`.

---

```c
void idt_set_gate(int n, uint32_t handler);
```

Install a handler at vector `n`. `handler` is the physical address of the
assembly stub. Uses the `kernel_cs` selector read during `idt_init`. Safe to
call after `idt_init` to install or replace handlers for specific vectors.

---

## 9. Planned handlers

| Vector                   | Exception/IRQ                           | Sprint   | Ticket    |
| ------------------------ | --------------------------------------- | -------- | --------- |
| 8, 10–14, 17, 21, 29, 30 | Error-code exceptions                   | Sprint 1 | SCRUM-135 |
| 13                       | General Protection Fault (diagnostic)   | Sprint 2 | —         |
| 14                       | Page Fault (CR2 dump + halt/kill LibOS) | Sprint 2 | SCRUM-17  |
| 33                       | IRQ1 / PS/2 keyboard                    | Sprint 1 | SCRUM-13  |
| 44                       | IRQ12 / PS/2 mouse                      | Sprint 2 | SCRUM-19  |
| 0x80                     | Syscall gate (DPL=3)                    | Sprint 3 | SCRUM-32  |

---

## 10. Design decisions and gotchas

**Why read `%cs` at runtime?** GRUB's GDT is not the same as a kernel-defined
GDT. If the kernel hard-codes `sel = 0x08` but GRUB used `0x10`, every `iret`
will GPF. Reading `%cs` at `idt_init` time — before any GDT switch — captures
whatever selector GRUB set up and uses it consistently. When the kernel installs
its own GDT (Sprint 5, SCRUM-45), `idt_init` should be re-called or the gates
updated with the new selector.

**`default_stub` is silent by design.** For non-error-code vectors (most CPU
exceptions below vector 8 and all hardware IRQs that fire before their handler
is installed), `default_stub` returns silently. This is preferable to halting on
any unhandled vector — a stray IRQ during boot shouldn't crash the system.
However, once the kernel is stable, replacing `default_stub` with a diagnostic
stub (print vector number + halt) would make debugging easier.

**Interrupt gates vs. trap gates.** ExoDoom uses interrupt gates (`0x8E`) for
all entries. Interrupt gates clear `IF` on entry, preventing nested interrupts.
Trap gates (`0x8F`) leave `IF` set. For the current single-threaded kernel with
no preemption, interrupt gates are the correct and safe choice. The syscall gate
(`0x80`) will also use an interrupt gate — the kernel disables interrupts on
syscall entry and re-enables them on exit.

**`pusha` / `popa` in stubs does not save segment registers.** This is fine for
ring-0-only operation. When the LibOS runs in ring 3, syscall entry involves a
privilege change that automatically switches stacks (via TSS) and the entry stub
must explicitly save/restore `DS`, `ES`, `FS`, `GS` and set them to kernel
selectors, then restore the user selectors on return. The `irq0_stub` pattern is
a starting point, not the final production stub.
