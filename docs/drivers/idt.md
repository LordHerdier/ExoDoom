# Driver: IDT, ISR, and Interrupt Handling

**Files:** `src/idt.c`, `src/idt.h`, `src/isr.s`, `src/io.h` **Status:** ✅
Partial — basic IDT wired, page fault handled, other exceptions absorbed
**Last updated:** 10 Sep 2026

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

`idt_init()` is called from `kernel_main` immediately after `memory_init()` —
early, and **above the `TESTING` branch** (SCRUM-17). Everything below that
line, the PMM and page-table construction and the test suite included, then
gets a serial diagnostic on a page fault instead of a silent loop. It is safe
this early: interrupt gates clear `IF`, the PIC is still masked as the BIOS
left it, and nothing calls `sti` until long after `pic_remap()`, so no hardware
IRQ can arrive on a not-yet-remapped vector.

```c
void idt_init(void) {
    idtp.limit = sizeof(struct idt_entry) * IDT_ENTRIES - 1;
    idtp.base  = (uint64_t)(uintptr_t)&idt;

    // Fill all 256 entries with default_stub
    for (int i = 0; i < IDT_ENTRIES; i++)
        idt_set_gate(i, (uintptr_t)default_stub);

    // ...then the error-code vectors, and finally vector 14, which wins
    for (unsigned i = 0; i < N_ERROR_CODE_VECTORS; i++)
        idt_set_gate(error_code_vectors[i], (uintptr_t)error_stub);

    idt_set_gate(14, (uintptr_t)pf_stub);

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
unhandled interrupt or exception — correct for non-error-code vectors. The 10
error-code vectors (see §7) are overridden after the fill loop — nine with
`error_stub`, vector 14 with `pf_stub` — so `default_stub` never actually runs
on those.

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

### `ALIGN_CALL_STACK` / `RESTORE_CALL_STACK`

```asm
.macro ALIGN_CALL_STACK
    push %rbp
    mov  %rsp, %rbp
    and  $-16, %rsp
.endm

.macro RESTORE_CALL_STACK
    mov  %rbp, %rsp
    pop  %rbp
.endm
```

The System V AMD64 ABI requires `%rsp % 16 == 0` immediately before a `call`.
The CPU does not guarantee any particular alignment of `%rsp` at interrupt
entry, and the byte-count of `PUSH_REGS` is not a reliable way to *derive*
alignment for every possible incoming `%rsp`. `ALIGN_CALL_STACK` saves the
exact pre-alignment `%rsp` via `%rbp` (callee-saved, so safe to use as a
temporary anchor) and masks `%rsp` to 16 bytes; `RESTORE_CALL_STACK` undoes
the masking and restores `%rbp`, leaving `%rsp` exactly where it was before
`ALIGN_CALL_STACK` ran, and `%rbp` restored to its original value.

### `irq0_stub`

```asm
.global irq0_stub
.extern irq0_handler

irq0_stub:
    PUSH_REGS
    ALIGN_CALL_STACK
    call irq0_handler
    RESTORE_CALL_STACK
    POP_REGS
    iretq
```

Installed on vector 32 (IRQ0 / PIT timer). After `irq0_handler` returns
(having sent EOI), the call stack is restored, registers are restored, and
`iretq` returns to the interrupted context.

### `irq1_stub`

```asm
.global irq1_stub
.extern irq1_handler

irq1_stub:
    PUSH_REGS
    ALIGN_CALL_STACK
    call irq1_handler
    RESTORE_CALL_STACK
    POP_REGS
    iretq
```

Installed on vector 33 (IRQ1 / PS/2 keyboard).

### `error_stub`

```asm
.global error_stub
error_stub:
    PUSH_REGS
    POP_REGS
    add  $8, %rsp        // discard the CPU-pushed error code
    iretq
```

Installed on nine of the ten error-code vectors (8, 10, 11, 12, 13, 17, 21,
29, 30 — see §7) by `idt_init`; vector 14 gets `pf_stub` instead (below). It
discards the CPU-pushed error code so `iretq` doesn't misread it as the return
`RIP`, and returns. This closes SCRUM-135: previously `default_stub` was
installed on these vectors and any one of them firing (most likely vector
13/GPF or 14/page fault) triple-faulted the machine with no diagnostic.

Note what "returns" means here: `error_stub` resumes **at the faulting
instruction**, which for a fault (rather than a trap) faults again
immediately. That is a silent infinite loop, not a recovery — acceptable as a
"don't triple-fault" measure for vectors that should never fire, and precisely
why vector 14 needed a real handler (SCRUM-17).

### `PUSH_ALL_REGS` / `POP_ALL_REGS`

`PUSH_REGS` saves only the caller-saved registers, which is all an IRQ handler
needs. A fault diagnostic wants the whole register file, laid out so a C
handler can read it as a struct. `PUSH_ALL_REGS` pushes all 15 GPRs in the
reverse of `exception_frame_t`'s field order (`src/fault.h`), so after the
pushes `%rsp` points at the `r15` field and the fields ascend from there into
the CPU-pushed error code and the `iretq` frame:

```c
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t error_code;                    // pushed by the CPU
    uint64_t rip, cs, rflags, rsp, ss;      // the iretq frame
} exception_frame_t;
```

Change one and change the other — see §10.

### `pf_stub`

```asm
.global pf_stub
.extern page_fault_handler

pf_stub:
    PUSH_ALL_REGS
    mov  %rsp, %rdi        // frame pointer — before ALIGN_CALL_STACK
    ALIGN_CALL_STACK
    call page_fault_handler
    RESTORE_CALL_STACK
    POP_ALL_REGS
    add  $8, %rsp          // discard the CPU-pushed error code
    iretq
```

Installed on vector 14 by `idt_init`, after the `error_stub` fill loop so it
wins (SCRUM-17). `mov %rsp, %rdi` must precede `ALIGN_CALL_STACK`: that macro
pushes `%rbp`, which would otherwise sit between `%rsp` and the frame being
passed. It leaves `%rdi` alone, so the pointer survives the alignment.

The tail after `call` is only reached when a `TESTING` hook asks the handler to
resume (see §9 and `src/fault.h`); on a real fault `page_fault_handler` halts
and never returns.

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

> ✅ **Resolved (SCRUM-135):** `default_stub` used to be installed on all of
> these vectors, so any one of them firing — most likely vector 13 (GPF) or
> vector 14 (page fault) — would immediately triple-fault the machine.
> `error_stub` is now installed on vectors 8, 10, 11, 12, 13, 17, 21, 29 and 30
> in `idt_init` and safely discards the error code before `iretq` (see §5).

> ✅ **Resolved (SCRUM-17):** vector 14 no longer absorbs its fault silently.
> `pf_stub` → `page_fault_handler` (`src/fault.c`) prints CR2, the decoded
> error code, the faulting `RIP`, `CS:RSP`, `RFLAGS` and what the live page
> tables say about the address, then halts. Sample output:
>
> ```
> === PAGE FAULT (#PF, vector 14) ===
>   cr2:       0x0000400000005000
>   error:     0x0000000000000002  (not-present write supervisor)
>   rip:       0x0000000000202BC1
>   cs:rsp:    0x0000000000000008:0x0000000000222F10
>   rflags:    0x0000000000010087
>   mapping:   none (no present entry along the walk)
>   context:   ring 0 (kernel) -- fatal
> === halted ===
> ```

Vector 13 (GPF) still uses `error_stub` and would benefit from the same
treatment — `exception_frame_t`, `ALIGN_CALL_STACK` and the decode helpers all
generalise; only the error-code decoding differs (a GPF pushes a segment
selector index, not the `P/W/U/RSVD/I-D` bitfield). See §9.

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

| Vector                    | Exception                | Handler status                                       |
| ------------------------- | ------------------------- | ---------------------------------------------------- |
| 8, 10–13, 17, 21, 29, 30  | Error-code exceptions     | ✅ `error_stub` (absorb + discard; SCRUM-135)         |
| 13                        | General Protection Fault | Planned: serial diagnostic + halt                    |
| 14                        | Page Fault               | ✅ `pf_stub` → `page_fault_handler` (SCRUM-17)        |
| 32                        | IRQ0 / Timer             | ✅ `irq0_stub` → `irq0_handler`                      |
| 33                        | IRQ1 / Keyboard          | ✅ `irq1_stub` → `irq1_handler`                      |
| 44                        | IRQ12 / Mouse            | ⬜ Sprint 2 (SCRUM-19)                                |

---

### The `TESTING` resume hook

`src/fault.h` exposes, under `-DTESTING` only:

```c
typedef int (*fault_hook_t)(exception_frame_t *f, uint64_t cr2);
void fault_set_hook(fault_hook_t h);
```

The handler calls the hook before printing anything; a nonzero return makes it
return instead of halting, so a test can fault deliberately and resume by
pointing `f->rip` somewhere safe. That is how `tests/kernel/test_fault_k.c`
proves the frame layout and the error-code bits from a real fault rather than a
simulation. A shipped kernel has no such hook, and no policy for resuming from
a page fault — that arrives with per-LibOS address spaces (SCRUM-47/48), where
a ring-3 fault terminates the LibOS instead of the machine.

---

## 10. Design decisions and gotchas

**`exception_frame_t` and `PUSH_ALL_REGS` are one definition in two places.**
The C struct in `src/fault.h` describes the exact bytes `pf_stub` pushes; the
assembler cannot check that for you. Reorder either without the other and the
handler reads registers from the wrong offsets — a wrong `rip` and a wrong
error code, with no compile-time complaint. `tests/kernel/test_fault_k.c`
asserts the saved `RIP` equals the known address of the faulting instruction,
which is what catches this.

**A fault fixup label cannot be a C label.** The first version of the fault
test took `&&label` and had the handler resume there. At `-O2` GCC only anchors
a label whose address is taken if a computed `goto` *in the same function*
targets it; with the jump living in the handler, GCC folded the label onto the
function prologue, so every "resume" restarted the test function and faulted
again — 24 bytes of stack leaked per iteration until `iretq` took a #GP and the
machine triple-faulted. The faulting accesses and their resume points now live
in `tests/kernel/fault_probe.s`, where both addresses are fixed at assembly
time.

**No IST stack for vector 14.** The handler runs on whatever stack was live at
the fault. If that stack is itself corrupt or unmapped, the handler faults and
the machine double-faults into `error_stub`. A dedicated IST stack needs a TSS,
which is SCRUM-46; until then the recursion guard in `page_fault_handler`
covers the common case (a fault raised while reporting a fault) but not a bad
stack pointer.

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

**Stack alignment before `call` is not free.** The System V AMD64 ABI requires
`%rsp % 16 == 0` immediately before a `call` instruction. The CPU does not
guarantee any particular alignment of `%rsp` at interrupt entry, so the exact
byte-count of `PUSH_REGS` (72 bytes) cannot be relied on to produce alignment
for every possible incoming `%rsp` — it was arithmetic coincidence, not an
enforced invariant. `irq0_stub`/`irq1_stub` wrap their `call` in
`ALIGN_CALL_STACK`/`RESTORE_CALL_STACK` (see §5), which saves the exact
pre-alignment `%rsp` via `%rbp` (callee-saved, so safe to use as a temporary
anchor), masks `%rsp` to 16 bytes for the call, and restores the saved value
afterward so `POP_REGS`/`iretq` see the untouched interrupt frame.

**Segment registers are largely irrelevant.** In 64-bit long mode, `CS` is
required for privilege level transitions but `DS`, `ES`, `SS` are ignored for
addressing (they use a flat 64-bit address space). The stubs do not save/restore
segment registers. Once the LibOS runs in ring 3, the syscall entry path (via
`syscall`/`sysret`) will handle the privilege transition.
