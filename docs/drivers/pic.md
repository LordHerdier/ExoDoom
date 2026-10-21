# Driver: PIC (8259A Programmable Interrupt Controller)

**Files:** `src/pic.c`, `src/pic.h`, `src/io.h` **Status:** ✅ Complete **Last
updated:** 20 Apr 2026

---

## Table of Contents

1. [Purpose](#1-purpose)
2. [Hardware background](#2-hardware-background)
3. [Why remapping is required](#3-why-remapping-is-required)
4. [Initialisation and remapping](#4-initialisation-and-remapping)
5. [IRQ masking](#5-irq-masking)
6. [End of Interrupt (EOI)](#6-end-of-interrupt-eoi)
7. [API reference](#7-api-reference)
8. [IRQ to vector mapping](#8-irq-to-vector-mapping)
9. [Design decisions and gotchas](#9-design-decisions-and-gotchas)

---

## 1. Purpose

The 8259A PIC arbitrates hardware interrupt lines (IRQs) and delivers them to
the CPU as interrupt vectors. ExoDoom uses it to receive timer ticks (IRQ0),
keyboard input (IRQ1), and PS/2 mouse events (IRQ12). The PIC must be remapped
at boot to avoid a collision with CPU exception vectors, and unneeded IRQs must
be masked to prevent spurious interrupts from reaching the kernel.

---

## 2. Hardware background

The PC uses a cascade of two 8259A PICs, each handling 8 IRQ lines:

```
        ┌──────────────┐         ┌──────────────┐
IRQ0 ─→ │              │         │              │ ←─ IRQ8  (RTC)
IRQ1 ─→ │  Master PIC  │         │  Slave PIC   │ ←─ IRQ9
IRQ2 ─→ │  (PIC1)      │←─ INT2 ─│  (PIC2)      │ ←─ IRQ10
IRQ3 ─→ │  0x20/0x21   │         │  0xA0/0xA1   │ ←─ IRQ11
IRQ4 ─→ │              │         │              │ ←─ IRQ12 (PS/2 mouse)
IRQ5 ─→ │              │   INT ──→  CPU          │ ←─ IRQ13 (FPU)
IRQ6 ─→ │              │         │              │ ←─ IRQ14 (Primary ATA)
IRQ7 ─→ │              │         │              │ ←─ IRQ15 (Secondary ATA)
        └──────────────┘         └──────────────┘
```

The slave PIC is connected to IRQ2 of the master. When any IRQ8–15 fires, the
slave signals the master on IRQ2, and the master signals the CPU. The CPU must
send an End of Interrupt (EOI) to both PICs for slave IRQs.

I/O port assignments:

| Port   | PIC    | Register          |
| ------ | ------ | ----------------- |
| `0x20` | Master | Command           |
| `0x21` | Master | Data (mask / ICW) |
| `0xA0` | Slave  | Command           |
| `0xA1` | Slave  | Data (mask / ICW) |

---

## 3. Why remapping is required

At power-on, the BIOS programs the master PIC to deliver IRQ0–7 as CPU vectors
`0x08`–`0x0F`, and the slave to deliver IRQ8–15 as vectors `0x70`–`0x77`.
Vectors `0x08`–`0x1F` are reserved by Intel for CPU exceptions:

| Vector | CPU Exception            |
| ------ | ------------------------ |
| 0x08   | Double Fault             |
| 0x0D   | General Protection Fault |
| 0x0E   | Page Fault               |

If the kernel enables interrupts without remapping, an innocent timer tick (IRQ0
→ vector `0x08`) will be misidentified as a Double Fault. The result is a triple
fault and reset. `pic_remap()` must be called before `sti`.

---

## 4. Initialisation and remapping

`pic_remap()` reinitialises both PICs using a four-word Initialization Command
Word (ICW) sequence. Each ICW must be written in order to the Data port after
the ICW1 trigger on the Command port.

```c
void pic_remap() {
    // ICW1: start initialisation, expect ICW4
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);  io_wait();
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);  io_wait();

    // ICW2: vector offset
    outb(PIC1_DATA, 0x20);  io_wait();  // Master IRQ0–7  → vectors 0x20–0x27
    outb(PIC2_DATA, 0x28);  io_wait();  // Slave  IRQ8–15 → vectors 0x28–0x2F

    // ICW3: cascade wiring
    outb(PIC1_DATA, 4);     io_wait();  // Master: slave on IRQ2 (bitmask 0b00000100)
    outb(PIC2_DATA, 2);     io_wait();  // Slave:  cascade identity = 2

    // ICW4: set 8086 mode (required for x86)
    outb(PIC1_DATA, ICW4_8086);  io_wait();
    outb(PIC2_DATA, ICW4_8086);  io_wait();

    // Mask all IRQs except IRQ0 (timer)
    outb(PIC1_DATA, 0xFE);  io_wait();  // 0b11111110 — only IRQ0 unmasked
    outb(PIC2_DATA, 0xFF);  io_wait();  // All slave IRQs masked
}
```

`io_wait()` writes a dummy byte to port `0x80` (the POST diagnostic port)
between PIC commands. Older PICs require a brief delay (~1 µs) between ICW
writes; `io_wait` provides this safely on both QEMU and real hardware.

**ICW constants:**

| Constant    | Value  | Meaning                        |
| ----------- | ------ | ------------------------------ |
| `ICW1_INIT` | `0x10` | Begin initialisation sequence  |
| `ICW1_ICW4` | `0x01` | ICW4 will follow               |
| `ICW4_8086` | `0x01` | 8086/88 mode (not MCS-80 mode) |

---

## 5. IRQ masking

The PIC's Interrupt Mask Register (IMR) is accessed via the Data port (`0x21`
for master, `0xA1` for slave). A `1` bit masks (suppresses) the corresponding
IRQ; a `0` bit allows it through.

After `pic_remap()`, the mask state is:

| IRQ  | Line            | Masked?           |
| ---- | --------------- | ----------------- |
| 0    | PIT timer       | **No** — unmasked |
| 1    | PS/2 keyboard   | **No** — unmasked (by `kbd_init()`) |
| 2    | Cascade (slave) | Yes               |
| 3–7  | Various         | Yes               |
| 8–15 | Slave IRQs      | Yes (all)         |

IRQs are unmasked as their drivers come online. To unmask an IRQ when its
handler is ready:

```c
// Read current master mask, clear bit 1
uint8_t mask = inb(PIC1_DATA);
outb(PIC1_DATA, mask & ~(1 << 1));
```

To unmask IRQ12 (PS/2 mouse, a slave IRQ), both the slave IRQ12 bit and the
master's cascade bit (IRQ2) must be clear:

```c
outb(PIC1_DATA, inb(PIC1_DATA) & ~(1 << 2));   // unmask cascade
outb(PIC2_DATA, inb(PIC2_DATA) & ~(1 << 4));   // unmask IRQ12 (bit 4 on slave)
```

---

## 6. End of Interrupt (EOI)

The PIC holds the CPU's attention on an IRQ until it receives an EOI command
(`0x20` written to the Command register). If EOI is not sent, the PIC will not
deliver any further interrupts at or below the current IRQ priority. **Every IRQ
handler must call `pic_send_EOI` before returning.**

```c
void pic_send_EOI(unsigned char irq) {
    if (irq >= 8) {
        outb(PIC2_COMMAND, 0x20);  // EOI to slave first for IRQ8–15
    }
    outb(PIC1_COMMAND, 0x20);      // EOI to master always
}
```

For slave IRQs (8–15), both the slave and master must receive EOI — the slave
for the specific IRQ, and the master for the cascade line (IRQ2). The order is
slave first, then master.

---

## 7. API reference

```c
void pic_remap(void);
```

Remap both PICs to vectors `0x20`–`0x2F`, set 8086 mode, and mask all IRQs
except IRQ0. Call once during `kernel_main` before `idt_set_gate(32, irq0_stub)`
and before `sti`.

---

```c
void pic_send_EOI(unsigned char irq);
```

Send End of Interrupt for the given IRQ number (0–15). Must be called at the end
of every hardware IRQ handler. Sends EOI to the slave PIC first for IRQ8–15,
then always to the master.

---

```c
uint16_t pic_read_irr(void);
uint16_t pic_read_isr(void);
uint16_t pic_read_imr(void);
uint16_t pic_read_elcr(void);
uint32_t pic_spurious_irq7_count(void);
uint32_t pic_spurious_irq15_count(void);
```

The first four return a register from both PICs as `slave << 8 | master`.
IRR/ISR are read through OCW3, and the read select is restored to IRR
afterwards. ELCR is the PIIX edge/level register at `0x4D0`/`0x4D1`. The two
counters report how many spurious interrupts §9's handlers have absorbed
(SCRUM-227).

---

## 8. IRQ to vector mapping

Post-remap mapping used throughout the codebase:

| IRQ | Vector    | Source                   | Handler status                  |
| --- | --------- | ------------------------ | ------------------------------- |
| 0   | 0x20 (32) | PIT channel 0 (timer)    | ✅ `irq0_stub` → `irq0_handler` |
| 1   | 0x21 (33) | PS/2 keyboard            | ✅ `irq1_stub` → `irq1_handler` |
| 2   | 0x22 (34) | Cascade — not a real IRQ | —                               |
| 4   | 0x24 (36) | COM1 serial (unused)     | —                               |
| 7   | 0x27 (39) | Spurious (master)        | ✅ `irq7_spurious_stub` (§9)    |
| 12  | 0x2C (44) | PS/2 mouse               | ⬜ Sprint 2 (SCRUM-19)          |
| 14  | 0x2E (46) | Primary ATA              | ⬜ Sprint 11 (SCRUM-102)        |
| 15  | 0x2F (47) | Spurious (slave)         | ✅ `irq15_spurious_stub` (§9)   |

---

## 9. Design decisions and gotchas

**Mask everything except IRQ0 at init.** It is safer to start with all IRQs
masked and unmask them one at a time as handlers are registered, rather than
unmasking everything and hoping no stray IRQ fires before a handler is ready. A
spurious unhandled IRQ with only `default_stub` installed will `iretq` cleanly
(for non-error-code vectors) but silently without sending EOI, which will lock
out that IRQ line permanently until the PIC is reset.

**`io_wait` between ICW writes.** Omitting these delays is a common source of
PIC initialisation bugs on real hardware. QEMU is more forgiving but the delays
are cheap and correct — leave them in.

**Spurious IRQs (IRQ7 and IRQ15) are handled, and IRQ15's case needs a
master EOI (SCRUM-227).** An 8259A answers with its lowest-priority vector
(IRQ7 on the master, IRQ15 on the slave) when the request it raised INTR for
is gone by the time the CPU acknowledges it. Spurious IRQs are delivered
whatever the IMR says, so `kernel_main` installs `irq7_spurious_stub` /
`irq15_spurious_stub` on vectors `0x27`/`0x2F` immediately after
`pic_remap()`, ahead of the `TESTING` branch. Both read the ISR
(`pic_read_isr()`) to tell a real IRQ from a spurious one:

- **IRQ7**: bit 7 clear means the master put nothing in service, so **no
  EOI**. A non-specific EOI there would retire whatever *is* in service.
- **IRQ15**: bit 15 clear means the slave put nothing in service, but the
  master acknowledged IRQ2 to cascade to it and *did*. So the handler sends
  **a master-only EOI**. "Spurious means no EOI" is wrong for this one.

This used to be a comment saying it was "not a practical concern". It was:
with IRQ15 on `default_stub`, master IRQ2 stayed in service for the rest of
the boot, which silently blocked every slave line (8–15) while IRQ0/IRQ1 kept
running at higher priority. In CI it showed up as SCRUM-227's flaky `hda_pcm`
suite. The trigger is specific to this platform:

- Firmware (SeaBIOS) sets the PIIX ELCR so the PCI lines (10 and 11) are
  **level-triggered** at the slave. `pic_remap()`'s ICW1 does not change
  that; `pic_read_elcr()` reads `0x0C00` on QEMU.
- The master sees the slave's output on IRQ2, which is **edge-triggered**.

So if IRQ11 rises while IF is clear, the master latches IRR2. If the HDA
driver then clears the source while still masked (`stream_program()` does,
on every stream start), the slave forgets the request but the master doesn't.
At the next `sti` the CPU acknowledges IRQ2 and the slave answers spurious
IRQ15. `test_hda_pcm_k.c`'s "a completion cleared while masked" case drives
that sequence on purpose and asserts the spurious count rose and IRQ2 is not
left in service. If a driver ever claims line 7 or 15, its gate replaces the
spurious stub, and its handler must make the same ISR check.

**IRQ2 (cascade) must stay unmasked when using slave IRQs.** When the PS/2 mouse
driver is added (SCRUM-19), IRQ12 is on the slave. Unmasking IRQ12 in the slave
IMR is not enough — IRQ2 on the master must also be unmasked, or the slave's
signal never reaches the CPU. The current mask (`0xFE` for master) has IRQ2
masked, which must be fixed before mouse support works.
