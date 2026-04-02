# Driver: PIT (8253/8254 Programmable Interval Timer)

**Files:** `src/pit.c`, `src/pit.h`, `src/sleep.c`, `src/sleep.h` **Status:** ✅
Complete (SCRUM-9, SCRUM-10) **Last updated:** 2 Apr 2026

---

## Table of Contents

1. [Purpose](#1-purpose)
2. [Hardware background](#2-hardware-background)
3. [Initialisation](#3-initialisation)
4. [IRQ0 handler](#4-irq0-handler)
5. [Tick counter and millisecond clock](#5-tick-counter-and-millisecond-clock)
6. [Sleep](#6-sleep)
7. [API reference](#7-api-reference)
8. [Future: PC speaker (channel 2)](#8-future-pc-speaker-channel-2)
9. [Design decisions and gotchas](#9-design-decisions-and-gotchas)

---

## 1. Purpose

The PIT provides the kernel's monotonic clock. Every millisecond, IRQ0 fires and
increments a tick counter. This counter feeds `kernel_get_ticks_ms()`, which is
used by:

- `kernel_sleep_ms()` — busy-wait delays during boot
- `DG_GetTicksMs()` — game loop timing (35 tics/sec)
- `DG_SleepMs()` — frame pacing in doomgeneric
- `exo_get_ticks` syscall — LibOS access to the clock

---

## 2. Hardware background

The Intel 8253/8254 PIT has three independent 16-bit countdown channels:

| Channel | IRQ  | Typical use                           |
| ------- | ---- | ------------------------------------- |
| 0       | IRQ0 | System timer — used by ExoDoom        |
| 1       | —    | Historically DRAM refresh, now unused |
| 2       | —    | PC speaker tone generation            |

All channels share a base input clock of **1.193182 MHz** (derived from the
original IBM PC's 14.31818 MHz crystal divided by 12).

Control is through port `0x43` (Mode/Command register) and the channel data
ports (`0x40`, `0x41`, `0x42`).

**Mode/Command register format (`0x43`):**

```
 7   6   5   4   3   2   1   0
┌───┬───┬───┬───┬───┬───┬───┬───┐
│SC1│SC0│RW1│RW0│ M2│ M1│ M0│BCD│
└───┴───┴───┴───┴───┴───┴───┴───┘
  channel  access  operating  BCD
  select   mode    mode
```

ExoDoom uses `0x36`: channel 0 (`SC=00`), lobyte/hibyte access (`RW=11`), mode 3
square wave (`M=011`), binary (`BCD=0`).

---

## 3. Initialisation

```c
void pit_init(uint32_t hz) {
    frequency = hz;
    uint32_t divisor = (1193180 + hz / 2) / hz;  // round to nearest integer

    outb(0x43, 0x36);              // channel 0, lobyte/hibyte, mode 3, binary
    outb(0x40, divisor & 0xFF);    // divisor low byte
    outb(0x40, (divisor >> 8) & 0xFF);  // divisor high byte
}
```

Called from `kernel_main` as `pit_init(1000)` — 1000 Hz, 1 ms per tick.

**Divisor calculation:** `1193180 / 1000 = 1193.18`, rounded to `1193`. The
`+ hz/2` term performs integer rounding rather than truncation, keeping the
actual tick rate as close to 1000 Hz as possible. The resulting frequency is
`1193182 / 1193 ≈ 1000.15 Hz` — an error of 0.015%, negligible for game timing.

**Mode 3 (Square Wave):** The counter decrements by 2 per clock and toggles the
output at half-count. The output is high for the first half and low for the
second. This generates the cleanest IRQ0 signal and is the standard mode for the
system timer.

**Why 1000 Hz?** One millisecond granularity matches what doomgeneric's
`DG_GetTicksMs` and `DG_SleepMs` expect, and makes the
`ticks * 1000 / frequency` arithmetic exact (no division needed since
`frequency == 1000`). It also matches `kernel_sleep_ms`'s unit directly.

---

## 4. IRQ0 handler

`irq0_handler()` is the C function called from the `irq0_stub` assembly stub in
`src/isr.s`. The stub saves all registers (`pusha`), calls `irq0_handler`,
restores them (`popa`), and executes `iret`.

```c
static volatile uint32_t ticks = 0;
static uint32_t frequency = 1000;
static volatile uint8_t print_pending = 0;

void irq0_handler(void) {
    ticks++;

    if (ticks % frequency == 0) {
        print_pending = 1;
    }

    pic_send_EOI(0);
}
```

Both `ticks` and `print_pending` are `volatile` — they are written inside an ISR
and read from the main loop, so the compiler must not cache them in registers or
reorder accesses across the ISR boundary.

`print_pending` is a one-bit flag used in the current boot demo to print a
timestamp once per second. `pit_take_print_pending()` reads and clears it
atomically (single-byte operations on x86 are atomic with respect to IRQs).

**EOI must be the last operation.** Sending EOI re-arms the PIC for the next
IRQ0. Doing it at the end of the handler (rather than the start) prevents a
second timer tick from nesting into the handler on a very fast machine, though
in practice QEMU will not generate two IRQ0s faster than the handler completes.

---

## 5. Tick counter and millisecond clock

```c
uint32_t kernel_get_ticks_ms(void) {
    return (uint64_t)ticks * 1000 / frequency;
}
```

The cast to `uint64_t` before the multiply prevents overflow. At 1000 Hz,
`ticks` overflows a `uint32_t` after ~49.7 days of continuous uptime — not a
concern for QEMU sessions, but the cast is correct practice.

When `frequency == 1000`, this simplifies to `ticks * 1` — the compiler will
optimise away the multiply and divide entirely.

**Reading `ticks` without disabling interrupts.** On x86, a 32-bit aligned read
is atomic with respect to IRQs (the CPU will not interrupt mid-read of a 32-bit
value on a 32-bit bus). This is safe for the single-reader/single-writer case
here. A multi-CPU system would need a lock, but ExoDoom is single-core.

---

## 6. Sleep

```c
void kernel_sleep_ms(uint32_t ms) {
    uint32_t start = kernel_get_ticks_ms();
    while ((kernel_get_ticks_ms() - start) < ms) {
        __asm__ volatile ("hlt");
    }
}
```

The `hlt` instruction suspends the CPU until the next interrupt. Without it, the
busy loop would consume 100% CPU and prevent IRQ0 from ever firing (interrupts
would still fire, but `hlt` makes the spin more power-efficient and is cleaner
behaviour under QEMU). After each `hlt`, the CPU handles the pending IRQ0 (which
increments `ticks`), then re-enters the loop to check the condition.

**Subtraction wrapping.** `kernel_get_ticks_ms() - start` uses unsigned
subtraction. If `ticks` wraps around from `0xFFFFFFFF` to `0`, the subtraction
wraps correctly in unsigned arithmetic: `(0 - 0xFFFFE000) = 0x00002000`, which
is the correct elapsed time. The sleep function is correct across the 49-day
overflow.

---

## 7. API reference

```c
void pit_init(uint32_t hz);
```

Program PIT channel 0 to fire IRQ0 at `hz` times per second. Call after
`pic_remap()` and `idt_set_gate(32, irq0_stub)`, before `sti`. The kernel uses
`pit_init(1000)`.

---

```c
uint32_t kernel_get_ticks_ms(void);
```

Return monotonic milliseconds elapsed since `pit_init` was called. Wraps after
~49.7 days. Safe to call from both main context and ISRs.

---

```c
uint8_t pit_take_print_pending(void);
```

Returns `1` and clears the flag if a full second has elapsed since the last
call; returns `0` otherwise. Used by the boot demo. Will be removed once the
LibOS is running.

---

```c
void kernel_sleep_ms(uint32_t ms);
```

Busy-sleep for at least `ms` milliseconds using `hlt` between ticks. Requires
interrupts enabled (`sti` must have been called). Accuracy is ±1 ms (one tick
period).

---

## 8. Future: PC speaker (channel 2)

Sprint 11 (SCRUM-98) adds PC speaker tone generation using PIT channel 2.
Channel 2's output is ANDed with port `0x61` bit 1 and drives the speaker.
Programming it does not interfere with channel 0.

```c
// To play a tone at freq Hz (pseudocode, not yet implemented):
uint32_t divisor = 1193180 / freq;
outb(0x43, 0xB6);              // channel 2, lobyte/hibyte, mode 3
outb(0x42, divisor & 0xFF);
outb(0x42, (divisor >> 8) & 0xFF);
outb(0x61, inb(0x61) | 0x03); // enable speaker gate + channel 2 gate

// To stop:
outb(0x61, inb(0x61) & ~0x03);
```

This will be exposed via `exo_sound_tone(freq, dur_ms)` and `exo_sound_stop()`
syscalls (SCRUM-100). The kernel manages the duration timer internally (likely
using the tick counter) so `exo_sound_tone` is non-blocking from the LibOS's
perspective.

---

## 9. Design decisions and gotchas

**`frequency` stored as a module-level variable.** `pit_init` saves `hz` into a
static `frequency` for use in `kernel_get_ticks_ms`. This means if `pit_init` is
called again with a different frequency (unlikely but possible during
debugging), the clock arithmetic will be consistent with the new rate.
Alternatively `frequency` could be hardcoded to 1000, but the current design
allows the rate to be changed at init time without touching the arithmetic.

**`print_pending` flag vs. a callback.** The current design polls
`pit_take_print_pending()` from the main loop. A cleaner future design would
register a callback via `pit_register_1s_callback(fn)` rather than exposing a
raw flag. This is low priority until the boot demo is replaced by the LibOS.

**No compensation for missed ticks.** If interrupts are disabled for more than 1
ms (e.g., during a long `cli` section), ticks will be missed and
`kernel_get_ticks_ms()` will under-report elapsed time. The kernel currently
holds `cli` only briefly (during IDT load and PIC init), so this is not an issue
in practice.

**Relationship to `DG_SleepMs`.** doomgeneric calls `DG_SleepMs` with small
values (typically 1–5 ms) to yield between frames. The LibOS implementation will
call `exo_get_ticks` in a loop with `hlt` — identical to `kernel_sleep_ms`. At
1000 Hz the resolution is exactly what doomgeneric expects.
