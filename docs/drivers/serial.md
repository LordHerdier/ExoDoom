# Driver: Serial (COM1)

**Files:** `src/serial.c`, `src/serial.h` **Status:** ✅ Complete **Last
updated:** 2 Apr 2026

---

## Table of Contents

1. [Purpose](#1-purpose)
2. [Hardware background](#2-hardware-background)
3. [Initialisation](#3-initialisation)
4. [Transmit path](#4-transmit-path)
5. [API reference](#5-api-reference)
6. [Design decisions and gotchas](#6-design-decisions-and-gotchas)

---

## 1. Purpose

COM1 is the kernel's sole output channel throughout development. Every
diagnostic message, memory map dump, test result, and kernel panic goes here.
QEMU maps COM1 to the host terminal via `-serial mon:stdio`, making it trivially
capturable by CI scripts.

The driver is output-only. There is no interrupt-driven receive path; the kernel
never reads from COM1 during normal operation.

---

## 2. Hardware background

The 8250/16550 UART (Universal Asynchronous Receiver/Transmitter) is exposed as
a set of 8 I/O port registers, base-addressed at `0x3F8` for COM1. Register
selection is controlled by the DLAB (Divisor Latch Access Bit) in the Line
Control Register.

Relevant registers used by this driver:

| Offset | DLAB | Name                      | Use                                        |
| ------ | ---- | ------------------------- | ------------------------------------------ |
| `+0`   | 0    | Transmit Holding Register | Write byte to send                         |
| `+0`   | 1    | Divisor Latch Low         | Baud rate divisor (low byte)               |
| `+1`   | 0    | Interrupt Enable Register | Disable all interrupts                     |
| `+1`   | 1    | Divisor Latch High        | Baud rate divisor (high byte)              |
| `+2`   | —    | FIFO Control Register     | Enable/clear FIFO                          |
| `+3`   | —    | Line Control Register     | Data bits, parity, stop bits; DLAB control |
| `+4`   | —    | Modem Control Register    | RTS/DSR, IRQ enable                        |
| `+5`   | —    | Line Status Register      | Transmit empty flags                       |

Baud rate is set by writing a 16-bit divisor to the latch registers while DLAB
is set. The UART's base clock is 1.8432 MHz, so divisor = 1,843,200 / (16 ×
baud).

---

## 3. Initialisation

`serial_init()` must be called once, early in `kernel_main`, before any output
functions are used.

```c
void serial_init(void) {
    outb(COM1 + 1, 0x00);  // Disable all interrupts
    outb(COM1 + 3, 0x80);  // Set DLAB to access baud rate divisor
    outb(COM1 + 0, 0x03);  // Divisor low byte  → 3 = 38400 baud
    outb(COM1 + 1, 0x00);  // Divisor high byte → 0
    outb(COM1 + 3, 0x03);  // Clear DLAB; 8 data bits, no parity, 1 stop bit (8N1)
    outb(COM1 + 2, 0xC7);  // Enable FIFO, clear TX/RX FIFOs, 14-byte threshold
    outb(COM1 + 4, 0x0B);  // RTS + DSR set; IRQ enabled on modem control
}
```

**Baud rate:** 38400. Divisor = 1,843,200 / (16 × 38400) = 3. This is the
standard development baud rate — fast enough for high-volume test output,
universally supported.

**FIFO:** The 16550's 16-byte FIFO is enabled with a 14-byte trigger threshold.
This means the UART will not raise a receive interrupt until 14 bytes have
accumulated — irrelevant here since receive interrupts are disabled, but the
FIFO does accelerate transmit bursts by buffering outgoing bytes in hardware.

**IRQs disabled:** `outb(COM1 + 1, 0x00)` disables all UART interrupts. The
driver uses polled (busy-wait) transmit only. This avoids needing an IRQ4
handler and keeps the driver self-contained.

---

## 4. Transmit path

All output ultimately flows through `serial_putc`, which polls the Line Status
Register until the Transmit Holding Register is empty, then writes one byte:

```c
static int serial_can_tx(void) {
    return inb(COM1 + 5) & 0x20;  // LSR bit 5: Transmit Holding Register Empty
}

void serial_putc(char c) {
    while (!serial_can_tx()) {}
    outb(COM1, (uint8_t)c);
}
```

`serial_flush()` waits for the stronger condition — bit 6 of the LSR (Transmit
Empty), meaning both the holding register _and_ the shift register are empty —
ensuring all bytes have left the UART before the caller proceeds:

```c
static int serial_tx_empty(void) {
    return inb(COM1 + 5) & 0x40;  // LSR bit 6: Transmitter Empty
}

void serial_flush(void) {
    while (!serial_tx_empty()) {}
}
```

`serial_flush()` must be called before `qemu_exit()` — the `isa-debug-exit`
device shuts the VM down immediately, and bytes still in the hardware FIFO will
be silently lost.

---

## 5. API reference

```c
void serial_init(void);
```

Configure COM1 at 38400 8N1, FIFO on, interrupts off. Call once at boot before
any other serial function.

---

```c
void serial_putc(char c);
```

Transmit a single byte. Busy-waits until the UART is ready. Safe to call from
interrupt context (no locking, but brief busy-wait may occur).

---

```c
void serial_print(const char* s);
```

Transmit a NUL-terminated string one byte at a time via `serial_putc`.

---

```c
void serial_print_hex(uint32_t num);
```

Print a 32-bit value as 8 uppercase hex digits (zero-padded). Example:
`serial_print_hex(0x200000)` → `"00200000"`.

---

```c
void serial_print_hex64(uint64_t num);
```

Print a 64-bit value as 16 uppercase hex digits. Used by `mmap_init` to print
base/length fields from the multiboot mmap, which are `uint64_t`.

---

```c
void serial_print_dec(uint32_t num);
```

Print a 32-bit unsigned integer in decimal. Handles the `0` case explicitly.
Digits are built in a temporary buffer reversed and then printed forward.

---

```c
void serial_print_u32(uint32_t val);
```

Alternate decimal formatter. Builds the string from the end of a 11-byte buffer
using pointer arithmetic, then calls `serial_print` on the result. Equivalent to
`serial_print_dec` but avoids an extra loop variable.

---

```c
void serial_flush(void);
```

Block until both the UART holding register and shift register are empty. Call
before `qemu_exit()` or any point where the VM may be halted.

---

## 6. Design decisions and gotchas

**Two decimal formatters (`serial_print_dec` vs `serial_print_u32`).** Both
exist and do the same thing. This is minor redundancy that should be resolved —
either drop one or consolidate into a single `serial_print_u32`. Neither is
wrong.

**No `\r\n` normalisation.** `serial_putc` sends bytes as-is. The kernel always
uses `\n` only in string literals. QEMU's terminal handles `\n` as a newline; on
real serial terminals `\r\n` may be needed. If connecting to real hardware via a
serial cable, consider adding a `\r` before each `\n` in `serial_putc`.

**Busy-wait blocks interrupts implicitly.** Because the driver polls rather than
using interrupts, any code calling `serial_print` during an ISR (e.g., the KUnit
test framework inside `irq0_handler`) will spin in the UART loop. At 38400 baud
a typical test output line takes ~2 ms — short enough not to miss ticks, but
worth knowing.

**`exo_serial_write` syscall (Sprint 3, SCRUM-53).** The LibOS will route
`printf`/`fprintf` through this syscall rather than calling `serial_print`
directly. The syscall implementation will validate that the buffer pointer is
within the LibOS's mapped address space before passing it to `serial_print`,
preventing a malicious or buggy LibOS from reading kernel memory via a crafted
print call.
