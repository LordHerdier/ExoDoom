# Driver: PS/2 Keyboard

**Files:** `src/ps2.c`, `src/ps2.h`, `src/kbd_ring.c`, `src/kbd_ring.h`,
`src/isr.s` (IRQ1 stub)
**Status:** ✅ IRQ1 handler and scan code processing complete (SCRUM-13,
SCRUM-14) / ✅ Ring buffer complete (SCRUM-18) **Last updated:** 3 Sep 2026

---

## Table of Contents

1. [Purpose](#1-purpose)
2. [Hardware background](#2-hardware-background)
3. [IRQ1 handler](#3-irq1-handler)
4. [Scan code Set 1 translation](#4-scan-code-set-1-translation)
5. [Modifier key state](#5-modifier-key-state)
6. [Event ring buffer](#6-event-ring-buffer)
7. [exo_kbd_poll syscall](#7-exo_kbd_poll-syscall)
8. [Doom keycode mapping](#8-doom-keycode-mapping)
9. [API reference](#9-api-reference)
10. [Design decisions and gotchas](#10-design-decisions-and-gotchas)

---

## 1. Purpose

The keyboard driver translates raw PS/2 scan codes into structured key events
and queues them for consumption by the LibOS via the `exo_kbd_poll` syscall.
doomgeneric's `DG_GetKey` dequeues these events and maps them to Doom keycodes
for the game engine.

---

## 2. Hardware background

The PS/2 keyboard controller is built into the motherboard (or emulated by
QEMU). It communicates through two I/O ports:

| Port   | Name           | Use                                               |
| ------ | -------------- | ------------------------------------------------- |
| `0x60` | Data           | Read scan code; write keyboard commands           |
| `0x64` | Status/Command | Read controller status; write controller commands |

When a key is pressed or released, the keyboard controller raises **IRQ1**,
places a scan code byte (or a sequence of bytes for extended keys) in the data
port at `0x60`, and waits for the CPU to read it. The controller's output buffer
must be read — if it is not, no further scan codes will be delivered.

**Status register (port `0x64`) bit 0** is the Output Buffer Full flag. It is
set when data is available at `0x60`. The IRQ1 handler should verify this before
reading, though in practice IRQ1 only fires when data is genuinely ready.

---

## 3. IRQ1 handler

The IRQ1 handler is responsible for reading the scan code from `0x60` as quickly
as possible and queuing it for later processing. It must send EOI to the PIC
before returning.

Two rules keep it from losing keys:

1. **Drain the output buffer, do not read a single byte.** IRQ1 is edge
   triggered — the controller signals the *arrival* of a byte, not the fact
   that bytes are waiting. If two scan codes are buffered and the handler
   consumes one, the leftover is never re-announced and the driver falls a byte
   behind for the rest of the session. The handler therefore loops while the
   status register's OBF bit is set.
2. **No serial I/O in the handler.** `serial_putc` spins on the UART's
   transmit-holding-register-empty bit; at 38400 baud a single logged keystroke
   costs milliseconds — long enough for the next scan codes to pile up behind
   it. Logging happens on the consumer side in `kbd_service()`.

**Assembly stub (`src/isr.s`):**

```asm
.global irq1_stub
.extern irq1_handler

irq1_stub:
    PUSH_REGS
    call irq1_handler
    POP_REGS
    iretq
```

The `PUSH_REGS`/`POP_REGS` macros save/restore caller-saved registers (`rax`,
`rcx`, `rdx`, `rsi`, `rdi`, `r8`–`r11`). In x86_64 long mode, `pusha`/`popa`
do not exist.

**C handler:**

```c
void ps2_irq1_handler(void) {
    for (unsigned i = 0; i < PS2_IRQ_DRAIN_MAX; i++) {
        uint8_t status = inb(0x64);

        if (!(status & PS2_STATUS_OBF))
            break;                      // nothing left buffered

        uint8_t data = inb(0x60);

        if (status & PS2_STATUS_AUX)
            continue;                   // mouse byte, belongs to IRQ12

        ps2_process_scancode(data);     // decode + enqueue, no I/O
    }

    pic_send_EOI(1);
}
```

`PS2_IRQ_DRAIN_MAX` (32) bounds the loop so a wedged controller that reports
OBF forever cannot spin inside the interrupt handler.

Registration in `kernel_main` (after `idt_init`, `pic_remap`):

```c
extern void irq1_stub(void);
idt_set_gate(33, (uintptr_t)irq1_stub);
kbd_init();  // unmasks IRQ1 in the PIC
```

**Status (SCRUM-13):** ✅ Complete — IRQ1 handler reads scan codes and processes
them.

---

## 4. Scan code Set 1 translation

PS/2 keyboards default to **Scan Code Set 1** (the original IBM PC set). Each
key has a **make code** (sent on key press) and a **break code** (sent on key
release). For most keys:

- Make code: a single byte in the range `0x01`–`0x58`
- Break code: make code `| 0x80` (bit 7 set)

Extended keys (arrow keys, Insert, Delete, Home, End, PgUp, PgDn, right Ctrl,
right Alt, keypad Enter, etc.) are prefixed with `0xE0`. Two-byte sequences:
`0xE0` followed by the key byte and `0xE0` followed by `key | 0x80` for release.

The translation table maps scan code → internal key enum. Selected entries:

| Make code     | Key                   |
| ------------- | --------------------- |
| `0x01`        | Escape                |
| `0x02`–`0x0B` | `1`–`9`, `0`          |
| `0x0E`        | Backspace             |
| `0x0F`        | Tab                   |
| `0x10`–`0x19` | `Q W E R T Y U I O P` |
| `0x1C`        | Enter                 |
| `0x1D`        | Left Ctrl             |
| `0x1E`–`0x26` | `A S D F G H J K L`   |
| `0x2A`        | Left Shift            |
| `0x2C`–`0x32` | `Z X C V B N M`       |
| `0x36`        | Right Shift           |
| `0x38`        | Left Alt              |
| `0x39`        | Space                 |
| `0x3B`–`0x44` | F1–F10                |
| `0xE0 0x48`   | Up arrow              |
| `0xE0 0x50`   | Down arrow            |
| `0xE0 0x4B`   | Left arrow            |
| `0xE0 0x4D`   | Right arrow           |

Translation is a `switch` over the raw make code byte (with the `0xE0` prefix
and the break bit stripped), plus a small state machine for prefixes:

```c
void ps2_process_scancode(uint8_t sc) {
    if (ps2_skip)     { ps2_skip--;        return; }  // tail of a Pause seq
    if (sc == 0xE1)   { ps2_skip = 2;      return; }  // Pause/Break prefix
    if (sc == 0xE0)   { ps2_extended = 1;  return; }  // extended prefix
    if (sc == 0xF0)   { ps2_break = 1;     return; }  // set 2 break prefix

    bool    release = (sc & 0x80) != 0;               // set 1 break bit
    uint8_t code    = sc & 0x7F;
    ...
}
```

**The break bit applies to extended keys too.** The up arrow makes `E0 48` and
breaks `E0 C8`; masking bit 7 off only for non-extended codes is what caused
arrow-key *releases* to be swallowed. Stripping it unconditionally is what lets
release events for extended keys reach the queue.

**Pause/Break is discarded.** It sends `E1 1D 45 E1 9D C5`, and `1D` decoded on
its own is Left Ctrl — a phantom Ctrl press that would then be stamped onto the
`modifiers` field of every subsequent event. Two bytes are swallowed after each
`E1`, which covers the sequence exactly.

**Acceptance criteria (SCRUM-14):** Pressing `A` logs `KEY_A`; shift/ctrl/alt
are tracked as modifier state.

---

## 5. Modifier key state

Modifier keys (Shift, Ctrl, Alt) are tracked as a bitmask updated on both press
and release events. This state is used by the Doom keycode translator (Sprint 4,
SCRUM-40) and potentially by the shell LibOS.

```c
#define MOD_LSHIFT  (1 << 0)
#define MOD_RSHIFT  (1 << 1)
#define MOD_LCTRL   (1 << 2)
#define MOD_RCTRL   (1 << 3)
#define MOD_LALT    (1 << 4)
#define MOD_RALT    (1 << 5)

static volatile uint8_t modifier_state = 0;
```

Updated in the IRQ1 handler whenever a modifier key is pressed or released, and
sampled into every queued event's `modifiers` field so a consumer that polls
late still sees the modifier state as it was at the moment of the keystroke,
not as it is now. Also readable directly via `ps2_get_modifier_state()`.

Right Ctrl and right Alt arrive as `E0`-prefixed copies of the left keys
(`E0 1D`, `E0 38`), so the extended flag is what selects `MOD_RCTRL` /
`MOD_RALT` over their left counterparts.

---

## 6. Event ring buffer

The ring buffer (`src/kbd_ring.c`, `src/kbd_ring.h`) decouples the IRQ1 handler,
which must be fast, from the consumer's polling rate. Without it, if two keys
are pressed between polls, the second event is lost.

It lives in its own module rather than as file-static state inside `ps2.c` so
that it is unit-testable without hardware — see
`tests/kernel/test_kbd_ring.c`.

```c
#define KBD_RING_CAPACITY 256u          // must be a power of 2
#define KBD_RING_MASK     (KBD_RING_CAPACITY - 1u)

typedef struct {
    uint8_t pressed;      // 1 = key down, 0 = key up
    uint8_t key;          // ps2_key_t
    uint8_t modifiers;    // MOD_* mask when the event was queued
} kbd_event_t;

typedef struct {
    kbd_event_t       slots[KBD_RING_CAPACITY];
    volatile uint16_t head;      // next write slot — producer owns
    volatile uint16_t tail;      // next read slot  — consumer owns
    volatile uint32_t dropped;   // events lost to overflow
} kbd_ring_t;
```

**Capacity.** A keystroke costs two events (down *and* up), so 256 slots absorb
128 keystrokes between polls. One slot is always left empty so that
`head == tail` means empty and `head + 1 == tail` means full without a shared
count field that both sides would have to write; usable depth is 255 events.

**`kbd_ring_push` (producer — IRQ1 only):**

```c
int kbd_ring_push(kbd_ring_t *ring, kbd_event_t event) {
    uint16_t head      = ring->head;
    uint16_t next_head = (head + 1u) & KBD_RING_MASK;

    if (next_head == ring->tail) {      // full: drop newest, count it
        if (ring->dropped != UINT32_MAX) ring->dropped++;
        return 0;
    }

    ring->slots[head] = event;
    kbd_ring_barrier();                 // slot written before head advances
    ring->head = next_head;
    return 1;
}
```

**`kbd_ring_pop` (consumer — `kbd_service` / `exo_kbd_poll`):**

```c
int kbd_ring_pop(kbd_ring_t *ring, kbd_event_t *out) {
    uint16_t tail = ring->tail;

    if (ring->head == tail) return 0;   // empty

    *out = ring->slots[tail];
    kbd_ring_barrier();                 // copied out before slot is released
    ring->tail = (tail + 1u) & KBD_RING_MASK;
    return 1;
}
```

The power-of-2 capacity means the wrap is a bitmask (`& 255`), branchless and
fast. The single-producer (IRQ1) / single-consumer (kernel loop or syscall)
pattern needs no locking on x86 — stores are not reordered with stores, loads
are not reordered with loads — but the *compiler* must be stopped from
reordering the slot write past the head store. `kbd_ring_barrier()` is an empty
`asm volatile` with a `"memory"` clobber, which is exactly that and nothing
more. Calling `kbd_ring_push` from ordinary kernel code, or popping from two
places, breaks the model and is not supported.

**Overflow policy: drop the newest, keep the history.** Overwriting the oldest
queued event would hand the consumer a stream with a hole in the middle;
dropping the incoming one leaves it an intact prefix. Every drop increments
`dropped` (saturating, so a long overflow cannot wrap it back to zero), and
`kbd_service` reports the count to serial so a stalled consumer is visible
rather than silent.

**Acceptance criteria (SCRUM-18):** Rapid typing does not drop keys; buffer
overflow handled gracefully (drop with no crash). Both are covered by the
`kbd_ring` and `ps2_decode` test suites — see
[docs/testing.md](../testing.md).

---

## 7. exo_kbd_poll syscall

Syscall number 6. Defined in the syscall spec as:

```c
// Kernel side:
int32_t exo_kbd_poll(kbd_event_t *event_out);
// Returns 1 if event available (fills *event_out), 0 if queue empty
```

The kernel validates that `event_out` is within the LibOS's mapped address space
before writing to it. The LibOS calls this in a polling loop from `DG_GetKey`:

```c
// LibOS DG_GetKey implementation:
int DG_GetKey(int *pressed, unsigned char *key) {
    kbd_event_t ev;
    if (exo_kbd_poll(&ev) == 0) return 0;
    *pressed = ev.pressed;
    *key     = doom_keycode(ev.key);   // translate to Doom keycode
    return 1;
}
```

`DG_GetKey` is called in a loop by the Doom game loop until it returns 0 (queue
empty), so all pending events are drained each frame.

---

## 8. Doom keycode mapping

Doom uses its own keycode constants (defined in `doomkey.h` in the doomgeneric
source). The LibOS translation layer (Sprint 4, SCRUM-40) maps internal key
enums to Doom keycodes:

| Internal key       | Doom keycode            | Notes           |
| ------------------ | ----------------------- | --------------- |
| `KEY_UP`           | `KEY_UPARROW` (0xAE)    | Move forward    |
| `KEY_DOWN`         | `KEY_DOWNARROW` (0xAF)  | Move backward   |
| `KEY_LEFT`         | `KEY_LEFTARROW` (0xAC)  | Turn left       |
| `KEY_RIGHT`        | `KEY_RIGHTARROW` (0xAD) | Turn right      |
| `KEY_CTRL`         | `KEY_FIRE` (0x80)       | Fire weapon     |
| `KEY_SPACE`        | `KEY_USE` (0x20)        | Use / open door |
| `KEY_SHIFT`        | `KEY_RSHIFT` (0xB2)     | Run             |
| `KEY_ESC`          | `KEY_ESCAPE` (27)       | Menu            |
| `KEY_ENTER`        | `KEY_ENTER` (13)        | Confirm         |
| `KEY_F1`–`KEY_F12` | `KEY_F1`–`KEY_F12`      | Function keys   |
| `KEY_A`–`KEY_Z`    | `'a'`–`'z'`             | Direct ASCII    |
| `KEY_0`–`KEY_9`    | `'0'`–`'9'`             | Direct ASCII    |

---

## 9. API reference

```c
void kbd_init(void);
```

Reset driver state and unmask IRQ1 in the PIC. Call after `idt_init()`,
`pic_remap()`, and `idt_set_gate(33, irq1_stub)`.

---

```c
void kbd_reset(void);
```

Empty the queue and clear decoder, modifier, and drop-counter state. Not safe
against a concurrent IRQ1 — call with IRQ1 masked or before `sti`.

---

```c
// Called from the IRQ1 handler only
void kbd_enqueue(kbd_event_t event);
void ps2_process_scancode(uint8_t scancode);
```

`ps2_process_scancode` decodes one byte of the scan code stream and enqueues the
resulting event, if any; it is also the entry point the tests drive directly.
`kbd_enqueue` stamps the current modifier state onto the event and pushes it.
Both drop the event when the queue is full, incrementing the drop counter.

---

```c
int kbd_dequeue(kbd_event_t *out);
int exo_kbd_poll(kbd_event_t *event_out);
```

Remove the oldest event from the ring buffer into `*out`. Returns 1 on success,
0 if empty (leaving `*out` untouched). `exo_kbd_poll` is the syscall-facing
wrapper and additionally rejects a NULL pointer; SCRUM-39 adds address-space
validation.

---

```c
uint16_t kbd_pending(void);
uint32_t kbd_dropped_count(void);
uint8_t  ps2_get_modifier_state(void);
```

Queue depth, total events discarded to overflow since the last reset, and the
live `MOD_*` mask.

---

```c
void kbd_service(void);
```

Consumer-side drain: pops every queued event, logs it to serial as
`KEY_x DOWN|UP (shift=n ctrl=n alt=n)`, and prints
`kbd: queue full, dropped N event(s)` when the drop counter has moved since the
last call. Call from ordinary kernel context — never from an interrupt handler,
since it blocks on the UART. `kernel_main` calls it from the idle loop:

```c
for (;;) {
    kbd_service();
    __asm__ volatile ("hlt");
}
```

---

## 10. Design decisions and gotchas

**Why a ring buffer rather than a single-event flag?** Doom's `DG_GetKey` is
called in a loop that drains all pending events per frame. If two keys are
pressed in one frame period (16 ms at 60 fps) and only one event is stored, the
second is lost. A 256-event buffer absorbs 128 keystrokes, far beyond any
realistic burst.

**Drop on overflow, never block.** The IRQ1 handler runs with interrupts
disabled. Blocking or sleeping inside it would freeze the system. Dropping
events when the buffer is full is the correct policy — a full buffer means the
consumer is not polling fast enough, which is a consumer bug, not a kernel bug.
The drop is *counted* rather than silent, so that bug is diagnosable instead of
looking like flaky hardware.

**A one-byte-per-IRQ handler loses keys, and the ring cannot save it.** The
queue only helps for events that were decoded in the first place. Because IRQ1
is edge triggered, a handler that reads a single byte while the controller has
two buffered leaves the second stranded — no further interrupt announces it.
Draining until OBF clears is what makes the ring's guarantee reach the
hardware.

**Serial logging belongs to the consumer, not the handler.** Printing one
keystroke over COM1 takes longer than the interval between scan codes during
fast typing, so logging inside IRQ1 caused exactly the drops the ring exists to
prevent. `ps2_process_scancode` does no I/O; `kbd_service` does all of it.

**`0xE0` extended key prefix requires state across interrupts.** The `extended`
flag persists between IRQ1 calls. This is a one-byte static variable updated
atomically. No locking needed since IRQ1 is not reentrant (the PIC won't deliver
another IRQ1 while the handler is running, since `IF` is cleared on interrupt
gate entry).

**Print-screen and Pause are special.** Print Screen sends `0xE0 0x2A 0xE0 0x37`
on press and `0xE0 0xB7 0xE0 0xAA` on release; neither `0x2A` nor `0x37` is in
the extended table, so it falls out as `KEY_UNKNOWN` and the translation table
handles it for free. Pause sends `0xE1 0x1D 0x45 0xE1 0x9D 0xC5` with no break
code, and the table is *not* enough there — `0x1D` is Left Ctrl, so decoding it
would leave a Ctrl key stuck down in the modifier mask. The decoder skips two
bytes after each `0xE1` instead.

**PS/2 mouse shares the controller.** Both the keyboard (IRQ1, port `0x60`) and
the PS/2 mouse (IRQ12, also port `0x60`) share the same data port. The
controller uses a multiplexer — keyboard data arrives without a prefix, mouse
data arrives after the host enables the mouse port via a controller command. The
IRQ handlers are on different vectors so there is no ambiguity about which
device generated a given interrupt.
