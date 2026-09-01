# Driver: PS/2 Keyboard

**Files:** `src/ps2.c`, `src/ps2.h`, `src/isr.s` (IRQ1 stub)
**Status:** ✅ IRQ1 handler and scan code processing complete (SCRUM-13,
SCRUM-14) / ⬜ Ring buffer (Sprint 2, SCRUM-18) **Last updated:** 20 Apr 2026

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
void irq1_handler(void) {
    uint8_t scancode = inb(0x60);
    kbd_enqueue(scancode);
    pic_send_EOI(1);
}
```

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

The translation table is a 128-entry array indexed by the raw make code byte
(ignoring `0xE0` prefix and the break bit). Extended key handling requires a
small state machine in the IRQ1 handler:

```c
static bool extended = false;

void irq1_handler(void) {
    uint8_t sc = inb(0x60);

    if (sc == 0xE0) {
        extended = true;
        pic_send_EOI(1);
        return;
    }

    bool pressed  = !(sc & 0x80);
    uint8_t base  = sc & 0x7F;
    key_t   key   = extended ? ext_scancode_to_key[base]
                              : scancode_to_key[base];
    extended = false;

    if (key != KEY_NONE)
        kbd_enqueue((kbd_event_t){ .pressed = pressed, .key = key });

    pic_send_EOI(1);
}
```

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

Updated in the IRQ1 handler whenever a modifier key is pressed or released. Read
by `exo_kbd_poll` and included in the event struct, or computed separately and
exposed as `exo_get_modifiers()`.

---

## 6. Event ring buffer

The ring buffer (Sprint 2, SCRUM-18) decouples the IRQ1 handler (which must be
fast) from the LibOS polling rate. Without it, if two keys are pressed between
polls, the second scan code is lost.

```c
#define KBD_BUFFER_SIZE 64   // must be a power of 2

typedef struct {
    uint8_t  pressed;    // 1 = key down, 0 = key up
    uint8_t  scancode;   // internal key enum value
} kbd_event_t;

static kbd_event_t kbd_buffer[KBD_BUFFER_SIZE];
static volatile uint8_t kbd_head = 0;  // written by IRQ1 handler
static volatile uint8_t kbd_tail = 0;  // read by exo_kbd_poll
```

**`kbd_enqueue` (called from IRQ1 handler):**

```c
static void kbd_enqueue(kbd_event_t event) {
    uint8_t next_head = (kbd_head + 1) & (KBD_BUFFER_SIZE - 1);
    if (next_head == kbd_tail) return;  // buffer full, drop event
    kbd_buffer[kbd_head] = event;
    kbd_head = next_head;              // atomic on x86 (byte write)
}
```

**`kbd_dequeue` (called from `exo_kbd_poll`):**

```c
static int kbd_dequeue(kbd_event_t *out) {
    if (kbd_head == kbd_tail) return 0;  // empty
    *out = kbd_buffer[kbd_tail];
    kbd_tail = (kbd_tail + 1) & (KBD_BUFFER_SIZE - 1);
    return 1;
}
```

The power-of-2 buffer size means the modulo operation reduces to a bitmask
(`& 63`), which is branchless and fast. The single-producer (IRQ1) /
single-consumer (main loop / syscall) pattern is safe without locks on x86 as
long as `head` and `tail` are byte-sized and the event struct is written
completely before the head pointer advances.

**Acceptance criteria (SCRUM-18):** Rapid typing does not drop keys; buffer
overflow handled gracefully (drop with no crash).

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
    *key     = doom_keycode(ev.scancode);  // translate to Doom keycode
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

The driver API is not yet finalised; the following reflects the planned design.

```c
void kbd_init(void);
```

Register the IRQ1 IDT gate and unmask IRQ1 in the PIC. Call after `idt_init()`
and `pic_remap()`.

---

```c
// Internal — called from IRQ1 handler only
void kbd_enqueue(kbd_event_t event);
```

Add an event to the ring buffer. Silently drops events when the buffer is full.

---

```c
int kbd_dequeue(kbd_event_t *out);
```

Remove the oldest event from the ring buffer into `*out`. Returns 1 on success,
0 if empty. Called by `exo_kbd_poll` after address validation.

---

## 10. Design decisions and gotchas

**Why a ring buffer rather than a single-event flag?** Doom's `DG_GetKey` is
called in a loop that drains all pending events per frame. If two keys are
pressed in one frame period (16 ms at 60 fps) and only one event is stored, the
second is lost. A 64-event buffer is large enough to absorb any realistic burst
of simultaneous key events.

**Drop on overflow, never block.** The IRQ1 handler runs with interrupts
disabled. Blocking or sleeping inside it would freeze the system. Silently
dropping events when the buffer is full is the correct policy — a full buffer
means the LibOS is not polling fast enough, which is a LibOS bug, not a kernel
bug.

**`0xE0` extended key prefix requires state across interrupts.** The `extended`
flag persists between IRQ1 calls. This is a one-byte static variable updated
atomically. No locking needed since IRQ1 is not reentrant (the PIC won't deliver
another IRQ1 while the handler is running, since `IF` is cleared on interrupt
gate entry).

**Print-screen and Pause are special.** Print Screen sends `0xE0 0x2A 0xE0 0x37`
on press and `0xE0 0xB7 0xE0 0xAA` on release. Pause sends
`0xE1 0x1D 0x45 0xE1 0x9D 0xC5` with no break code. Neither is needed for Doom
and can be safely ignored by the translation table (mapped to `KEY_NONE`).

**PS/2 mouse shares the controller.** Both the keyboard (IRQ1, port `0x60`) and
the PS/2 mouse (IRQ12, also port `0x60`) share the same data port. The
controller uses a multiplexer — keyboard data arrives without a prefix, mouse
data arrives after the host enables the mouse port via a controller command. The
IRQ handlers are on different vectors so there is no ambiguity about which
device generated a given interrupt.
