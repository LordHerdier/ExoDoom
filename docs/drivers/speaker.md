# Driver: PC speaker (PIT channel 2)

**Files:** `src/speaker.c`, `src/speaker.h` **Status:** ✅ Complete (SCRUM-98)
**Last updated:** 25 Sep 2026

---

## Table of Contents

1. [Purpose](#1-purpose)
2. [Hardware background](#2-hardware-background)
3. [Playing a tone](#3-playing-a-tone)
4. [Duration without blocking](#4-duration-without-blocking)
5. [API reference](#5-api-reference)
6. [Testing](#6-testing)
7. [Design decisions and gotchas](#7-design-decisions-and-gotchas)

---

## 1. Purpose

Ring-0 tone generator for the Audio epic (SCRUM-146). It is the layer the
`exo_sound_tone`/`exo_sound_stop` syscalls (SCRUM-100) sit on, and through
them Doom's `sound_module_t` (SCRUM-101). It knows how to make the speaker
beep at a frequency for a duration; it knows nothing about who asked.

## 2. Hardware background

The speaker is driven by PIT channel 2's `OUT` pin, gated through system
control port `0x61`:

| `0x61` bit | Meaning                                                     |
| ---------- | ----------------------------------------------------------- |
| 0          | Channel 2 `GATE` input — the counter only runs while set     |
| 1          | Speaker data enable — connects channel 2 `OUT` to the speaker |
| 5 (read)   | Current level of channel 2 `OUT`                             |

Channel 2 in mode 3 (square wave) with reload value `1193180 / f` produces a
square wave at `f` Hz. Channels 0 and 2 share only the command port `0x43`,
and every command byte selects its own channel, so programming channel 2
never disturbs the 1000 Hz system timer on channel 0 (`src/pit.c`).

## 3. Playing a tone

```c
outb(0x43, 0xB6);                 // channel 2, lobyte/hibyte, mode 3, binary
outb(0x42, divisor & 0xFF);
outb(0x42, divisor >> 8);
outb(0x61, inb(0x61) | 0x03);     // gate on + speaker on
```

Stopping clears bits 0–1 of `0x61` again (read-modify-write, so the other
bits — NMI/parity masks — are left alone). The divisor is rounded to
nearest, the same way `pit_init()` rounds channel 0's.

Accepted range is `SPEAKER_MIN_HZ`–`SPEAKER_MAX_HZ` = **19–20000 Hz**. The
floor is where the divisor still fits the 16-bit counter (18 Hz would need
66288); the ceiling is the top of human hearing. Out-of-range requests return
`SPEAKER_EINVAL` and change nothing — the tone already playing, if any,
keeps playing.

## 4. Duration without blocking

`speaker_tone(freq, dur_ms)` records a deadline of
`kernel_get_ticks_ms() + dur_ms` and returns immediately. `irq0_handler()`
calls `speaker_tick(now)` on every tick, which closes the gate once the
deadline has passed (wrap-safe signed compare, same idiom as
`kernel_sleep_until_ms()`). Resolution is therefore the PIT's 1 ms.

`dur_ms == 0` means "until stopped": no deadline is armed. A new tone always
replaces the old one, deadline included — a short timed tone followed by an
untimed one is not cut off by the first one's deadline.

Because the stop happens in IRQ0, a timed tone only ends while interrupts are
enabled. That is always the case on a normal boot; in the `TESTING` build,
the tests that depend on it open their own `sti`/`kernel_sleep_ms`/`cli`
window.

## 5. API reference

```c
void     speaker_init(void);                        // silence; called from kernel_main after pit_init()
int      speaker_tone(uint32_t freq_hz, uint32_t dur_ms); // SPEAKER_OK / SPEAKER_EINVAL
void     speaker_stop(void);
int      speaker_is_playing(void);
void     speaker_tick(uint32_t now_ms);             // from irq0_handler()
uint16_t speaker_divisor_for(uint32_t freq_hz);     // exposed for tests
```

`speaker_tone()`/`speaker_stop()` save and restore `RFLAGS.IF` around the
port writes (`pushfq; cli` … `popfq`), so they are safe from syscall context
(IF already clear) and from ring-0 code with interrupts on, and never turn
interrupts on for a caller that had them off.

## 6. Testing

`tests/kernel/test_speaker_k.c` (suite `speaker`) checks the hardware rather
than the driver's own bookkeeping:

- the 8254 read-back command reports channel 2 in mode 3, lobyte/hibyte;
- a latched channel 2 count is non-zero and never above the programmed
  divisor;
- port `0x61` bit 5 (channel 2 `OUT`) is seen both high and low during a
  1 kHz tone — the square wave really is being generated;
- a 20 ms tone is silent after 40 ms of real IRQ0s, a 60 ms tone is still
  sounding after 20 ms, and an untimed tone survives;
- out-of-range frequencies are rejected without touching the speaker.

QEMU's `pc` machine emulates port `0x61` (`hw/audio/pcspk.c`) with or
without an audio backend, so none of this needs `-audiodev`. To actually
*hear* it, boot with e.g.
`-audiodev pa,id=snd0 -machine pcspk-audiodev=snd0`.

## 7. Design decisions and gotchas

**No ownership check.** The speaker has no binding table (compare
`src/fb_binding.c`, `src/disk_binding.c`). That is a SCRUM-100 decision, not
this driver's — this layer is ring-0 only, and ring 3 cannot reach ports
`0x42`/`0x43`/`0x61` itself (IOPL 0, no I/O permission bitmap).

**One voice.** The PC speaker is a single square-wave channel. Mixing
several Doom sound effects is SCRUM-101's policy problem (last-started
wins), not something the hardware can do.
