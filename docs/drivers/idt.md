# Driver: IDT, ISR, and Interrupt Handling

**Files:** `src/idt.c`, `src/idt.h`, `src/isr.s`, `src/io.h` **Status:** ✅
Partial — basic IDT wired, dedicated handlers in progress **Last updated:** 20
Apr 2026

---

## Table of Contents

1. [Purpose](#1-purpose)
2. [x86_64 interrupt mechanics](#2-x86_64-interrupt-mechanics)
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

## 2. x86_64 interrupt mechanics

When an interrupt or exception fires on x86_64, the CPU:

1. Finishes the current instruction (for most exceptions; some are precise).
2. If a privilege level change is required (ring 3 → ring 0), the CPU loads the
   kernel stack from the TSS (via the IST if specified) and switches stacks.
3. Pushes `SS`, `RSP`, `RFLAGS`, `CS`, and `RIP` onto the (kernel) stack — the
   return context.
4. For certain exceptions: pushes an additional **error code** (see §7).
5. Looks up the vector number in the IDT to find the handler address.
6. Clears `IF` (disables further interrupts) for interrupt gates.
7. Transfers control to the handler.

The handler returns via `iretq`, which pops `RIP`, `CS`, `RFLAGS`, `RSP`, and
`SS` in one atomic operation, restoring the interrupted context. If an error
code was pushed, the handler **must pop it before `iretq`** — otherwise `iretq`
reads the error code as the return `RIP` and the stack is misaligned, causing a
triple fault.

---

## 3. IDT structure

The x86_64 IDT is an array of 256 **16-byte** gate descriptors. ExoDoom uses
64-bit interrupt gates exclusively (`type_attr = 0x8E`):

```c
struct idt_entry {
    uint16_t offset_low;   // handler address bits 0–15
    uint16_t selector;     // code segment selector (0x08)
    uint8_t  ist;          // IST index (0 = no IST)
    uint8_t  type_attr;    // gate type + DPL + present bit
    uint16_t offset_mid;   // handler address bits 16–31
    uint32_t offset_high;  // handler address bits 32–63
    uint32_t reserved;     // must be 0
} __attribute__((packed));
```

**`type_attr = 0x8E` breakdown:**

```
 7   6   5   4   3   2   1   0
┌───┬───┬───┬───┬───┬───┬───┬───┐
│ P │DPL│DPL│ 0 │ 1 │ 1 │ 1 │ 0 │
└───┴───┴───┴───┴───┴───┴───┴───┘
  1   0   0   0   1   1   1   0
  │   └───┘       └───────────┘
present DPL=0    64-bit int gate
```

- **P=1**: gate is present
- **DPL=00**: descriptor privilege level 0 (ring 0 only — the future syscall
  gate will use `syscall`/`sysret` instead of an IDT entry)
- **Type=1110**: 64-bit interrupt gate (clears `IF` on entry)

The IDT pointer struct passed to `lidt`:

```c
struct idt_ptr {
    uint16_t limit;   // sizeof(idt) - 1
    uint64_t base;    // virtual address of idt array
} __attribute__((packed));
```

Note: the base is 64-bit (8 bytes) in long mode, making the IDT pointer 10
bytes total.

---

## 4. Initialisation

`idt_init()` is called from `kernel_main` after `memory_init()` and before
`pic_remap()`:

```c
void idt_init(void) {
    idtp.limit = sizeof(struct idt_entry) * IDT_ENTRIES - 1;
    idtp.base  = (uint64_t)(uintptr_t)&idt;

    // Fill all 256 entries with default_stub
    for (int i = 0; i < IDT_ENTRIES; i++)
        idt_set_gate(i, (uintptr_t)default_stub);

    idt_load(&idtp);
}
```

**Hardcoded selector `0x08`.** The code segment selector is hardcoded to `0x08`
— the kernel code segment in our own GDT (defined in `boot.s`). Unlike the
previous i386 version, we no longer read `%cs` at runtime because we control
the GDT entirely (GRUB's GDT is replaced during the long mode transition).

**`idt_set_gate` splits 64-bit addresses.** The 64-bit handler address is split
across three fields: `offset_low` (bits 0–15), `offset_mid` (bits 16–31), and
`offset_high` (bits 32–63).

**Fill before load.** All 256 entries are initialised with `default_stub` before
`lidt` is called. This ensures there is no window where a vector is present in
the CPU's view but points to uninitialised memory.

---

## 5. Assembly stubs

All handler stubs live in `src/isr.s` (`.code64`).

### `idt_load`

```asm
.global idt_load
idt_load:
    lidt (%rdi)       // first argument in RDI (System V AMD64 ABI)
    ret
```

Called from C as `idt_load(&idtp)`. The `lidt` instruction accepts a 10-byte
memory operand (the `idt_ptr` struct: 2-byte limit + 8-byte base) and loads the
IDT register.

### `default_stub`

```asm
.global default_stub
default_stub:
    iretq
```

Installed on all 256 IDT entries during `idt_init`. Silently returns from any
unhandled interrupt or exception — correct for non-error-code vectors, **fatal
for error-code vectors** (see §7 and §10).

### Register save/restore macros

In 64-bit long mode, `pusha`/`popa` do not exist. IRQ stubs manually save and
restore the caller-saved registers:

```asm
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
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rax
.endm
```

### `irq0_stub`

```asm
.global irq0_stub
.extern irq0_handler

irq0_stub:
    PUSH_REGS
    call irq0_handler
    POP_REGS
    iretq
```

Installed on vector 32 (IRQ0 / PIT timer). After `irq0_handler` returns
(having sent EOI), registers are restored and `iretq` returns to the
interrupted context.

### `irq1_stub`

```asm
.global irq1_stub
.extern irq1_handler

irq1_stub:
    PUSH_REGS
    call irq1_handler
    POP_REGS
    iretq
```

Installed on vector 33 (IRQ1 / PS/2 keyboard).

---

## 6. Adding a new IRQ handler

To add a handler for a new IRQ (e.g., IRQ12 for the mouse at vector 44):

**Step 1 — Write the assembly stub in `src/isr.s`:**

```asm
.global irq12_stub
.extern irq12_handler

irq12_stub:
    PUSH_REGS
    call irq12_handler
    POP_REGS
    iretq
```

**Step 2 — Declare the extern in the relevant C file or header:**

```c
extern void irq12_stub(void);
```

**Step 3 — Implement the C handler:**

```c
void irq12_handler(void) {
    uint8_t data = inb(0x60);
    // ... process mouse packet byte ...
    pic_send_EOI(12);
}
```

**Step 4 — Register the gate and unmask the IRQ, after `idt_init()` and
`pic_remap()`:**

```c
idt_set_gate(44, (uintptr_t)irq12_stub);

// Unmask IRQ2 (cascade) and IRQ12 (mouse)
outb(0x21, inb(0x21) & ~(1 << 2));    // unmask cascade on master
outb(0xA1, inb(0xA1) & ~(1 << 4));    // unmask IRQ12 on slave
```

The order matters: register the gate before unmasking the IRQ. If the IRQ fires
before the gate is installed, `default_stub` will handle it silently — but it's
cleaner to eliminate the race.

---

## 7. Exception vectors and error codes

x86_64 reserves vectors 0–31 for CPU exceptions. Of these, a subset pushes a
64-bit error code onto the stack before transferring to the handler.
`default_stub`'s bare `iretq` will misread this error code as the return `RIP`,
misalign the stack, and triple-fault.

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

> ⚠️ **Open issue (SCRUM-135):** `default_stub` is currently installed on all of
> these vectors. Any one of them firing — most likely vector 13 (GPF) or vector
> 14 (page fault) — will immediately triple-fault the machine. A dedicated
> `error_stub` must be installed on vectors 8, 10, 11, 12, 13, 14, 17, 21, 29,
> and 30 before any paging refinement work begins.

**The `error_stub` fix:**

```asm
.global error_stub
error_stub:
    add $8, %rsp    // discard the 8-byte error code pushed by the CPU
    iretq
```

This is the minimal fix. More useful would be a handler that prints diagnostic
information before halting, particularly for vectors 13 and 14:

```asm
.global gpf_stub
.extern gpf_handler

gpf_stub:
    PUSH_REGS
    mov %rsp, %rdi          // pass stack frame pointer as first arg
    call gpf_handler
    POP_REGS
    add $8, %rsp            // pop error code
    iretq
```

---

## 8. API reference

```c
void idt_init(void);
```

Fill all 256 IDT entries with `default_stub`, load the IDT register. Call once
from `kernel_main` before `pic_remap()` and before any `idt_set_gate` calls.

---

```c
void idt_set_gate(int n, uintptr_t handler);
```

Install a handler at IDT entry `n`. Splits the 64-bit `handler` address across
`offset_low`, `offset_mid`, and `offset_high`. Sets selector to `0x08`,
`type_attr` to `0x8E`, IST to 0, reserved to 0.

---

```c
extern void idt_load(struct idt_ptr *ptr);
```

Assembly function. Loads the IDT register from the 10-byte struct at `ptr`
(passed in `%rdi` per System V AMD64 ABI). Defined in `src/isr.s`.

---

## 9. Planned handlers

| Vector | Exception                | Handler status                                       |
| ------ | ------------------------ | ---------------------------------------------------- |
| 13     | General Protection Fault | Planned: serial diagnostic + halt                    |
| 14     | Page Fault               | Planned: print CR2, error code, faulting RIP; halt   |
| 32     | IRQ0 / Timer             | ✅ `irq0_stub` → `irq0_handler`                      |
| 33     | IRQ1 / Keyboard          | ✅ `irq1_stub` → `irq1_handler`                      |
| 44     | IRQ12 / Mouse            | ⬜ Sprint 2 (SCRUM-19)                                |

---

## 10. Design decisions and gotchas

**Hardcoded selector `0x08` instead of reading `%cs`.** The previous i386
version read `%cs` at runtime because GRUB's GDT was used directly. In the
x86_64 version, the boot trampoline loads our own GDT before entering long mode,
so the code segment selector is always `0x08`. Hardcoding it is simpler and
eliminates a potential source of confusion.

**16-byte IDT entries.** x86_64 IDT entries are 16 bytes (vs. 8 in 32-bit
mode) because handler addresses are 64 bits. The `offset_high` field holds bits
32–63 and the `reserved` field must be zero. A common bug is forgetting to zero
the reserved field, which causes the CPU to read garbage as part of the handler
address.

**No `pusha`/`popa` in long mode.** The 64-bit instruction set removes these
instructions. IRQ stubs must manually push/pop caller-saved registers. The
`PUSH_REGS`/`POP_REGS` macros save `rax`, `rcx`, `rdx`, `rsi`, `rdi`,
`r8`–`r11`. Callee-saved registers (`rbx`, `rbp`, `r12`–`r15`) are preserved
by the C handler per the System V AMD64 ABI.

**`-mno-red-zone` is mandatory.** x86_64 System V ABI defines a 128-byte "red
zone" below RSP that functions can use without adjusting RSP. Interrupt handlers
would corrupt this zone if the compiler assumes it is safe. The kernel is
compiled with `-mno-red-zone` to prevent the compiler from using the red zone.

**Segment registers are largely irrelevant.** In 64-bit long mode, `CS` is
required for privilege level transitions but `DS`, `ES`, `SS` are ignored for
addressing (they use a flat 64-bit address space). The stubs do not save/restore
segment registers. Once the LibOS runs in ring 3, the syscall entry path (via
`syscall`/`sysret`) will handle the privilege transition.
