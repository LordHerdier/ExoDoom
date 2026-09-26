# Driver: Intel High Definition Audio (CORB/RIRB + stream DMA)

**Files:** `src/hda.c`, `src/hda.h` **Status:** ✅ Complete (SCRUM-210)
**Last updated:** 25 Sep 2026

---

## Table of Contents

1. [Purpose](#1-purpose)
2. [Hardware background](#2-hardware-background)
3. [Controller bring-up](#3-controller-bring-up)
4. [CORB / RIRB: talking to the codec](#4-corb--rirb-talking-to-the-codec)
5. [The codec walk](#5-the-codec-walk)
6. [Stream DMA: BDL and the stream descriptor](#6-stream-dma-bdl-and-the-stream-descriptor)
7. [The completion interrupt](#7-the-completion-interrupt)
8. [API reference](#8-api-reference)
9. [Testing](#9-testing)
10. [Design decisions and gotchas](#10-design-decisions-and-gotchas)
11. [Where the samples come from](#11-where-the-samples-come-from)
12. [Streaming mode: the software mixer](#12-streaming-mode-the-software-mixer)

---

## 1. Purpose

Before this ticket the kernel's entire audio capability was the PC speaker
(`src/speaker.c`, SCRUM-98): one square-wave voice, one bit of output, driven
by PIT channel 2. That is enough for Doom's SFX-as-beeps mapping
(`src/doom_sfx_tone.c`) and nothing else — no PCM, no mixing, no sample data.

HDA is the audio controller that still exists on real x86 hardware. SB16 is
ISA and effectively extinct; AC'97 is nearly so. QEMU emulates HDA as
`-device intel-hda`, so one driver covers both CI and metal. It also masters
its own DMA off a buffer descriptor list, which means there is no 8237-style
DMA controller to build first — the original SB16 plan's largest hidden
dependency.

What this driver does: find the controller on the PCI bus, reset it, stand up
the CORB/RIRB command ring, walk the codec for an output converter and an
output pin, unmute them, and play a synthesized square-wave tone out of a
64 KiB cyclic buffer by DMA, with buffer completion signalled through the
controller's legacy PCI interrupt line.

What it deliberately does not do: expose anything to ring 3. There is no
`exo_sound_pcm` and no HDA ownership binding — those are separate tickets this
one unblocks. Like `src/pci.c`, `src/ata.c` and `src/speaker.c`, this file has
no notion of a caller.

---

## 2. Hardware background

HDA is three pieces of hardware with three different access mechanisms, and
keeping them straight is most of what makes the driver readable.

| Piece | What it is | How software reaches it |
|---|---|---|
| **Controller** | A PCI function, class `0x0403`. BAR 0 is a memory-mapped register block (16 KiB on QEMU's ICH6). | Ordinary MMIO loads/stores, once `src/pci.c` has sized and mapped the BAR. |
| **Codec** | A separate chip on the HDA serial link. Holds the DACs, amplifiers and physical jacks as a tree of *widgets*. | Not memory-mapped at all. Software sends 4-byte **verbs** through the controller's CORB ring and reads replies from its RIRB ring — both of which are DMA buffers in ordinary system memory. |
| **Stream** | A DMA engine in the controller's register block, one *stream descriptor* per stream. | Programmed through MMIO, but the data it moves is named by a **BDL** (buffer descriptor list) in system memory. |

Two consequences worth internalising:

- **Every codec operation is asynchronous and goes through RAM.** There is no
  "codec register" to poke. A verb is a dword written into a ring the
  controller reads by DMA; its answer is a pair of dwords the controller
  writes into another ring by DMA.
- **Stream descriptors are not symmetric.** Input streams occupy the first
  `GCAP.ISS` descriptor slots and output streams follow them, so the first
  *output* descriptor index is `GCAP.ISS` — 4 on QEMU. Hardcoding 0 addresses
  a capture stream.

Everything the controller touches by DMA — CORB, RIRB, BDL, samples — comes
from `alloc_pages_contig_owned()` (`src/page_alloc.c`), never `kmalloc`: the
bump allocator is finished once `page_alloc_init()` has run (see `CLAUDE.md`),
and the BDL names *physical* addresses, so the pages have to be contiguous.
The kernel map is an identity map, so the pointer the driver writes through is
the address the controller is given.

---

## 3. Controller bring-up

`hda_init()` runs from `kernel_main` right after `pci_init()`/`pci_dump()`,
ahead of the `TESTING` branch like every other driver a KUnit suite drives.

```
pci_find_class(0x04, 0x03)      -> the controller's pci_device_t
pci_bar_read(dev, 0, &bar)      -> size and decode BAR 0
pci_bar_assign(...) if base==0  -> only if firmware left it unprogrammed
pci_enable_device(dev)          -> bus master + memory space
pci_bar_map(&bar)               -> identity-map the window, PCD (uncached)
```

Bus mastering is not optional here: the controller reads the BDL and the
sample buffer itself. Without it every register still reads back correct and
nothing ever plays — which is why `tests/kernel/test_hda_k.c` asserts the
command register bit directly rather than trusting `pci_enable_device()`.

Then the reset, per HDA 1.0a §4.2.2:

1. Stop `INTCTL`, `CORBCTL`, `RIRBCTL` so no DMA engine is mid-transfer.
2. Clear `GCTL.CRST`, poll until it reads 0.
3. Set `GCTL.CRST`, poll until it reads 1.
4. **Wait at least 521 µs** (25 link frames) before reading `STATESTS`.

Step 4 is correctness, not politeness: codecs report themselves in `STATESTS`
during that window, and reading it early finds an empty link on hardware that
is merely slow — indistinguishable from having no codec at all. `STATESTS` is
then a bitmap of codec addresses, and the driver takes the lowest set bit.

Every poll loop in the file is bounded (`HDA_POLL_TRIES`) and returns
`HDA_ETIMEDOUT`. `hda_init()` is on the boot path; a controller that never
answers must cost a diagnostic line, not a hung CI boot. `kernel_main` treats
the whole call as soft-fail, the same way it treats `ata_init()`.

---

## 4. CORB / RIRB: talking to the codec

Two rings, 256 entries each, one page apiece out of a single two-page
contiguous allocation. CORB entries are 4 bytes (a verb), RIRB entries are 8
(a response plus a response-extension dword saying which codec answered).

A verb is:

```
 31..28   27..20        19..0
 codec    widget node   verb id + payload
```

The verb field is split two ways depending on the verb: 12 bits of id and 8 of
payload (`GET_PARAMETER` = `0xF00`, so `0xF0000 | param`), or 4 bits of id and
16 of payload (`SET_CONVERTER_FORMAT` = `0x2`, so `0x20000 | format`). The
`HDA_VERB_*` constants in `src/hda.h` are pre-shifted into position, so a call
site is just `HDA_VERB_x | payload`.

`hda_codec_verb()` sends one and waits for its answer:

1. Clear `RIRBSTS` (see the gotcha below — this is load-bearing).
2. Write the verb at `CORB[(wp + 1) % 256]`, then publish the new `CORBWP`.
3. Poll `RIRBWP` until it differs from the driver's read pointer.
4. Read the response dword; clear `RIRBSTS`.

**Responses are polled, not interrupt-driven** (`RIRBCTL.RINTCTL` stays
clear). A verb round trip is synchronous by nature, and `hda_codec_verb()` is
called from `hda_init()` with interrupts possibly off. The interrupt this
driver does take is the stream completion one, which is genuinely
asynchronous.

---

## 5. The codec walk

Widgets form a two-level tree, discovered entirely through
`GET_PARAMETER`:

```
node 0 (root)
  param 0x04 NODE_COUNT  -> [start, count) of function groups
  function group node
    param 0x05 FUNC_TYPE   -> 0x01 = audio function group
    param 0x04 NODE_COUNT  -> [start, count) of widgets
    widget node
      param 0x09 AUDIO_WIDGET_CAP, bits 23:20 -> type
                               0x0 = audio output (DAC)
                               0x4 = pin complex
      param 0x0C PIN_CAP, bit 4 -> pin can do output
      param 0x12 OUT_AMP_CAP    -> amp step count, for the gain
```

The driver records the first DAC and the first output-capable pin. On QEMU's
`hda-output` codec that is node 2 and node 3, under function group node 1.

It then configures them:

| Verb | Value | Why |
|---|---|---|
| `SET_POWER_STATE` (`0x705`) | `0x00` (D0) | A codec parked in D3 accepts every verb below and produces no sound. |
| `SET_CONVERTER_FORMAT` (`0x2`) | `0x0011` | 48 kHz, 16-bit, 2 channels. Must match `SDnFMT`. |
| `SET_CONVERTER_STREAM_CHANNEL` (`0x706`) | `(tag << 4) \| 0` | Binds the DAC to stream tag 1. Without it the DAC ignores the link entirely, format or no format. |
| `SET_AMP_GAIN_MUTE` (`0x3`) | `0xB000 \| gain` | Output amp, both channels, unmuted. `gain` is the widget's own maximum, i.e. `OUT_AMP_CAP`'s **step count** (bits 14:8) — *not* the step-size field next to it at bits 22:16. A hardcoded value is either inaudible or clipped depending on the codec. Skipped for a widget that advertises no output amp. |
| `SET_PIN_WIDGET_CONTROL` (`0x707`) | `0x40` | Enables output on the pin. |
| `SET_EAPD_BTL` (`0x70C`) | `0x02` | External amplifier power-down is active-low; without it a laptop's speakers stay silent while every register reads correct. Only sent if the pin advertises EAPD. |

---

## 6. Stream DMA: BDL and the stream descriptor

The sample buffer is 16 contiguous pages = 64 KiB = 16384 stereo 16-bit frames
≈ 341 ms at 48 kHz. `fill_tone()` writes a square wave into it, integer-only
(every kernel C file compiles `-mno-sse` and cannot use a `double`; a square
wave needs no trigonometry anyway).

The buffer is **cyclic** — the controller loops the BDL forever — so the wave
has to close on itself. `fill_tone()` rounds to a whole number of periods and
derives each sample's phase from that count, which snaps the playable
frequency to a multiple of 48000 / 16384 ≈ 2.93 Hz and reports the result
through `hda_tone_hz()`. Anything else leaves a partial period at the wrap
point, i.e. an audible click 2.9 times a second.

The BDL has **two** entries, each covering half the buffer, both with IOC set.
Two rather than one for two reasons: the spec requires `LVI ≥ 1`, so a
single-entry list is illegal; and two entries halve the time between "DMA is
running" and the first observable completion interrupt.

The output stream descriptor at `0x80 + 0x20 * GCAP.ISS` is then programmed:

```
SDnCTL   = 0                       ; stop
SDnCTL   = SRST                    ; reset, then clear and wait for 0
SDnSTS  <- BCIS|FIFOE|DESE         ; clear whatever the last run latched
SDnCBL   = 65536                   ; whole cyclic buffer, bytes
SDnLVI   = 1                       ; *index* of the last entry, not the count
SDnFMT   = 0x0011                  ; must match SET_CONVERTER_FORMAT
SDnBDPL/U= BDL page                ; physical, split low/high
SDnCTL   = (tag << 20) | IOCE      ; stream tag 1, interrupt on completion
INTCTL  |= GIE | (1 << stream)     ; master enable + this descriptor
SDnCTL  |= RUN                     ; go
```

`SDnLPIB` then reports the controller's own byte position in the buffer as it
reads, which is the cheapest available proof that DMA is really happening
rather than merely configured.

`hda_init()` programs all of this but starts nothing. `kernel_main` plays the
boot test tone from the normal-boot tail, *after* `sti` — not next to
`hda_init()` — because the duration is enforced by `hda_tick()` from
`irq0_handler`, so a tone started with interrupts still masked would sound
until whenever they were enabled. That placement also keeps the tone out of
`TESTING` builds, which exit through `run_tests()` and drive
`hda_play_tone()` themselves.

---

## 7. The completion interrupt

There is no MSI support in this kernel and no I/O APIC, so the controller's
interrupt arrives as a legacy PCI **INTx** on an 8259 line. That line is not a
constant: firmware routes it and records the result in the function's PCI
`Interrupt Line` register (config offset `0x3C`, added to `src/pci.h` for
this). QEMU routes HDA to IRQ 11 in practice; the driver reads it and never
assumes it.

`hda_init()` therefore does its own IDT wiring, rather than `kernel_main`
doing it the way IRQ0/IRQ1 are done:

```c
idt_set_gate(32 + line, (uintptr_t)irq_hda_stub);   /* vector first  */
if (line >= 8) pic_unmask_irq(2);                   /* slave cascade */
pic_unmask_irq(line);                               /* unmask second */
```

The ordering matches `kernel_main`'s IRQ1/`kbd_init()` precedent, and for the
same reason: the other way round, an interrupt arriving in between lands on
`idt_init()`'s `default_stub`, whose bare `iretq` sends no EOI and wedges the
line for the rest of the boot.

`irq_hda_stub` (`src/isr.s`) is a clone of `irq1_stub` calling
`hda_irq_handler()`, which reads `INTSTS`, and if this stream's bit is set:
latches any `FIFOE`/`DESE` for a test to inspect, clears the status bits, and
counts the completion. It always sends EOI, including for an interrupt whose
bit is clear — INTx is shareable, and the PIC set an in-service bit either
way.

`hda_init()` refuses to wire IRQ 0, 1 or 2 (`HDA_ENOIRQ`) and carries on
without completion interrupts instead. There is no shared-IRQ dispatch in this
kernel — one vector, one handler — so a controller routed onto one of those
would have this driver's stub replace the timer's or the keyboard's. A silent
tone is recoverable; a dead timer is not.

Timed tones follow `src/speaker.c` exactly: `hda_play_tone()` records a
deadline and returns, and `hda_tick()` — called from `irq0_handler()` right
after `speaker_tick()` — clears `SDnCTL.RUN` once it passes. That is the
property a future `exo_sound_pcm` needs in order not to make a LibOS sleep
through its own audio.

---

## 8. API reference

| Function | Returns | Notes |
|---|---|---|
| `hda_init()` | `HDA_OK` / negative | Full bring-up. Bounded, never halts, soft-fails in `kernel_main`. |
| `hda_present()` | `int` | 1 once a controller is found and mapped. |
| `hda_play_tone(hz, ms)` | `HDA_OK` / `HDA_EINVAL` / `HDA_ENODEV` | Non-blocking. `ms == 0` = until `hda_stop()`. Rejects outside `[20, 20000]` Hz, leaving the stream untouched. |
| `hda_stop()` | — | Clears `SDnCTL.RUN`. |
| `hda_is_playing()` / `hda_tone_hz()` | `int` / `uint32_t` | State, and the frequency actually programmed after snapping. |
| `hda_tick(now_ms)` | — | Called from `irq0_handler()`; ends a timed tone. Wrap-safe. |
| `hda_irq_handler()` | — | Called from `irq_hda_stub`. |
| `hda_codec_verb(codec, node, verb, &resp)` | `HDA_OK` / `HDA_ETIMEDOUT` | One synchronous verb round trip through CORB/RIRB. |
| `hda_reg_read8/16/32(off)` | value | Global registers; 0 when not present. |
| `hda_sd_read32/16(off)`, `hda_sd_status()` | value | This driver's output stream descriptor. |
| `hda_bar_base()`, `hda_codec_addr()`, `hda_dac_node()`, `hda_pin_node()`, `hda_stream_index()`, `hda_irq_line()` | value | Discovered topology. `0` (or `0xFF` for codec/IRQ) means "none". |
| `hda_corb_phys()`, `hda_rirb_phys()`, `hda_bdl_phys()`, `hda_buf_phys()` | `uint64_t` | The DMA pages, for asserting the registers point at them. |
| `hda_stream_position()`, `hda_irq_count()`, `hda_stream_errors()` | `uint32_t` / `uint8_t` | `SDnLPIB`, completion count, latched `FIFOE`/`DESE`. |
| `hda_dump()` | — | One-screen serial summary; called from `kernel_main`. |

Status codes: `HDA_ENODEV`, `HDA_ENOCODEC`, `HDA_ETIMEDOUT`, `HDA_ENOMEM`,
`HDA_EINVAL`, `HDA_ENOIRQ`, `HDA_EMAP`, `HDA_ENOOUT`.

---

## 9. Testing

`tests/kernel/test_hda_k.c`, registered as the `hda` suite in
`tests/kernel/test_runner.c` right after `pci` (which it depends on). Eleven
tests, driving the real driver against real emulated hardware — there is no
way to fake an MMIO register block or a codec's verb responses from inside the
kernel, and a mock would prove nothing about the one thing this ticket is for.

`docker-test`/`docker-ci` pass
`-device intel-hda -device hda-output,audiodev=snd0 -audiodev none,id=snd0`,
so every assertion is unconditional: a build that stops finding the
controller, the codec or a DAC is a regression here, not a machine without a
sound card.

The two that matter most:

- **BDL DMA runs** — `SDnLPIB` changes across a ~400 ms window with interrupts
  enabled. This is what separates "registers hold the right values" from "the
  controller is reading our memory."
- **Completion IRQ fires** — `hda_irq_count()` increases and
  `hda_stream_errors()` stays 0. That is the whole path: IOC on a BDL entry →
  `SDnSTS.BCIS` → INTx → the 8259 line out of PCI config space → vector
  32 + line → `irq_hda_stub` → `hda_irq_handler`.

Both need interrupts, so they `sti` for exactly their own wait and `cli`
afterwards — `kernel_main` does no blanket `sti` before `run_tests()`, and no
suite may leak an IRQ0 window into the next one (see
`test_doomgeneric_timer_k.c`'s header). The suite's cleanup calls
`hda_stop()`, because these are the only tests that leave a DMA engine and an
unmasked IRQ line running while they measure.

**Audibility** is not CI-provable: headless Docker has no audio backend, and
`-audiodev none` runs the entire DMA path and discards the samples. What CI
asserts is the chain up to the point samples leave RAM.

Two ways to go further, both worth knowing:

- **Listen to it.** `kernel_main` plays a 440 Hz / 600 ms tone on a normal
  boot once `hda_init()` has succeeded and interrupts are on
  (`HDA_BOOT_TONE_HZ`/`_MS`). Use `make run` (host QEMU) with a real backend
  in place of `-audiodev none,id=snd0` — `pipewire`, `pa`, `alsa` or `sdl`,
  whichever the host runs; `qemu-system-x86_64 -audiodev help` lists what the
  binary supports.
- **Capture it, which is reproducible and needs no speaker.**
  `-audiodev wav,id=snd0,path=out.wav` writes what the codec received to a
  WAV file on exit. That is how the amp-gain bug in §10 was found and fixed:
  the file should hold a ~440 Hz square wave at peak amplitude
  `HDA_TONE_AMPLITUDE` (8192), and before the fix it held the right frequency
  at a peak of 322.

---

## 10. Design decisions and gotchas

- **`OUT_AMP_CAP`'s step *count* is bits 14:8; bits 22:16 are the step
  *size*.** Reading the wrong one gave the most instructive bug of this
  ticket, because it produced a driver that looked perfect: every register
  read back correct, DMA ran, the completion interrupt fired, all eleven tests
  passed — and the tone came out about 28 dB down, plainly visible in a capture
  and completely inaudible on a speaker. QEMU's codec reports a step size of 3
  against 74 steps, so a gain of 3/74 scaled to 10/255 of full output: the
  measured peak was 322 where the driver writes 8192. Nothing inside the
  kernel can measure loudness, so the regression test asserts the codec's own
  gain readback equals its advertised maximum (and explicitly *not* the
  step-size field). The general lesson: for an audio path, "every register is
  correct" and "you can hear it" are genuinely different claims, and only a
  capture settles the second one — `-audiodev wav,id=snd0,path=out.wav` is the
  cheapest way to get one.

- **`SDnSTS` is a byte register at descriptor offset `0x03`, and only an
  access to `0x03` can clear it.** It shares one 32-bit location with
  `SDnCTL`, so a dword read at offset `0x00` conveniently returns the status
  in bits 31:24 — and a dword write carrying a status bit is silently
  dropped, because the control register's writable mask does not cover those
  bits. This is not cosmetic: `SDnSTS.BCIS` is what holds INTx asserted after
  a buffer completion, and INTx is level-triggered, so a clear that does not
  clear means the handler is re-entered the instant it returns, forever. This
  cost a full debugging cycle during SCRUM-210 and is exactly what it looks
  like: every register reads correct, the boot wedges with no fault and no
  output.
- **`RINTCNT` is flow control, not just an interrupt divisor.** The controller
  stops fetching from the CORB once `RINTCNT` unread responses have
  accumulated, and resumes only when software clears `RIRBSTS.RINTFL`. The
  obvious `RINTCNT = 1` therefore makes the command ring work *exactly once*
  unless every caller clears `RIRBSTS` first. The driver does both: `0xFF`, so
  the throttle stays far from a driver that sends one verb at a time, and a
  clear on both sides of every command. QEMU models this faithfully
  (`intel_hda_corb_run()`), so it is not a hardware-only concern.
- **The reset handshakes cannot be strictly asserted.** Both `CORBRP.RST` and
  `SDnCTL.SRST` are specified as "set it, wait to read it back set, clear it,
  wait to read it back clear", but QEMU completes each reset inside the
  register write and never presents the intermediate state. Linux tolerates
  the same thing (`azx_reset_corb_rp()`, `azx_stream_reset()` both use bounded
  loops that simply fall through). The driver waits, ignores that wait's
  result, and checks the *final* value — which is the part that matters.
- **The first output stream descriptor index is `GCAP.ISS`, read at runtime.**
  Input descriptors come first in the register block. Hardcoding 0 programs a
  capture stream, which on QEMU means a stream that never advances.
- **`-device intel-hda` alone is not enough.** It instantiates the controller
  with no codec attached, so `STATESTS` reads 0 and there is no widget tree.
  SCRUM-209 only needed the PCI function to exist; every QEMU invocation in
  the `Makefile` and `docker/Dockerfile.qemu` now also passes
  `-device hda-output,audiodev=snd0 -audiodev none,id=snd0`.
- **DMA memory comes from `alloc_pages_contig_owned()`, not `kmalloc`.** The
  bump allocator is finished once `page_alloc_init()` has run, and a BDL names
  physical addresses, so contiguity is a requirement rather than a
  convenience. The upper halves of the CORB/RIRB/BDL base registers are
  written as 0 deliberately, not by omission: on a 256 MiB machine every page
  is below 4 GiB, and the code that computes them (`phys >> 32`) is correct
  for a machine where they are not.
- **No FPU or XMM state is involved.** This driver is integer-only and names
  no XMM register, so the "nothing saves FPU/XMM on kernel entry" invariant
  (`docs/syscall_spec.md` §3.4a) is untouched — including inside
  `hda_irq_handler`, which runs from a real hardware interrupt.
- **No MSI, no capability-list walk.** `docs/drivers/pci.md` §8's note about
  the capability list at offset `0x34` still stands: nothing walks it. INTx is
  what this ticket wires, and it is sufficient because the PIC and IDT already
  exist.
- **No ownership or binding layer.** Ring 0 only, like `src/ata.c` before
  SCRUM-188. `exo_sound_pcm` and an HDA binding table are separate tickets.

---

## 11. Where the samples come from

This driver plays whatever is in its cyclic buffer. `kernel_main` puts a
synthesized 440 Hz tone there at boot (SCRUM-210); Doom's real sound effects
are decoded by a separate module, `src/doom_dmx.c/h` (SCRUM-211). The two are
joined by `src/pcm_mixer.c/h` and the streaming mode described in §12
(SCRUM-212).

The gap that mixer has to close, which is the reason it exists at all:

| | DMX lump (`doom_dmx_t`) | HDA stream |
|---|---|---|
| Sample format | 8-bit **unsigned** | 16-bit signed |
| Channels | mono | 2 (`HDA_BYTES_PER_FRAME` = 4) |
| Sample rate | **five different rates**, see below | `HDA_SAMPLE_RATE_HZ` = 48000, fixed |

`doom_dmx_to_s16()` closes the first row and nothing else — and the mixer does
not call it, because it needs the conversion fused with the gain and the
interpolation rather than as a separate pass over a copy. The second row is a
duplication. The third is the one that needed actual work, and it is worse
than the folklore suggests: DMX lumps are commonly described as "always
11025 Hz", and freedoom2 v0.13.0's 109 `DS*` lumps are 67 at 22050 Hz, 38 at
11025, 2 at 17990, 1 at 16000 and 1 at 44100. `tests/kernel/test_doom_dmx_k.c`
asserts that histogram exactly, so a mixer written against a single assumed
input rate will be caught by that test's neighbours rather than by two thirds
of Doom's effects quietly playing an octave low.

`doom_dmx_t.samples` points into the identity-mapped IWAD module and is
read-only for the life of the mount, so the mixer reads through it directly
rather than copying — see `src/doom_dmx.h` for the rest of the contract,
including the 16 padding samples stripped at each end. The corollary is a
lifetime rule: **a voice must not outlive its mount**, so anything calling
`doom_wad_unmount()` calls `pcm_mixer_reset()` first.

---

## 12. Streaming mode: the software mixer

`src/pcm_mixer.c/h` (SCRUM-212) holds up to `PCM_MIXER_VOICES` = 8 voices —
matching `src/doom/s_sound.c`'s `snd_channels` — each reading a decoded DMX
lump at its own rate, summed and clipped into the interleaved stereo frames
this driver DMAs. It is the *second* of two mutually exclusive modes on the
same stream descriptor and the same 64 KiB buffer; `hda_play_tone()` is the
first.

### 12.1 Why the BDL is diced differently

A tone is written once and loops forever, so `HDA_BDL_ENTRIES` = 2 is plenty.
For a stream that is rewritten continuously, **the entry size is the latency**:
nothing started can be heard until the DMA engine reaches an entry written
after the request. At two entries that is 8192 frames, ~171 ms — a gunshot a
sixth of a second after the trigger, an order of magnitude worse than Doom's
frame period.

So streaming programs `HDA_BDL_ENTRIES_PCM` = 16 entries over the *same*
buffer: `HDA_PCM_ENTRY_FRAMES` = 1024 frames, ~21 ms each, ~47 completion
interrupts a second. Nothing about the buffer or its physical pages changes —
only how finely it is diced, and therefore how often the handler refills a
slice. `bdl_program(entries)` rewrites the list in place and records the count;
`stream_program()` derives `LVI` from that count. **Each mode programs its own
layout on entry**, so neither has to undo the other's on exit, and a tone
started after a stream (or the reverse) is correct without a caller sequencing
them.

### 12.2 The refill, and why it trails the read head

`pcm_refill()` runs inside `hda_irq_handler()`, after the `SDnSTS` clear —
deliberately after, because that write is what deasserts the level-triggered
INTx line and the mixing is by far the longest thing the handler does.

It reads `SDnLPIB` to find the entry the controller is inside now, and renders
every entry from its own cursor up to (not including) that one. Those entries
have been consumed, so overwriting them puts the fill almost a full buffer
ahead of the read head — which is what makes a 21 ms slice safe rather than
marginal. Catching up in a **loop**, rather than assuming exactly one entry per
interrupt, is what makes a coalesced or missed completion cost one longer
render instead of a permanent hole in the output; the loop is bounded by the
entry count.

Because the handler runs with IF clear, `pcm_mixer_render()` sees a stable
voice table without a lock of its own. The mixer's *mutators* — called from
ordinary kernel context — do their own `cli`/`sti` pair for the few
instructions in which a voice is half-written, the same primitive and the same
reason as this file's `irq_save()`/`irq_restore()`.

### 12.3 The stream never stops

`hda_pcm_start()` leaves the stream running with no voices sounding, emitting
the silence `pcm_mixer_render()` writes for an empty mixer. Stopping per effect
would reset `SDnLPIB` and reintroduce exactly the start-up latency the 16-entry
split exists to remove, and a restart mid-DMA is audible as a click. Idle cost
is one interrupt every ~21 ms writing zeros.

That `pcm_mixer_render()` writes *every* frame it is given — silence included,
rather than leaving frames it does not reach — is what guarantees no leftover
of a previous tone or a finished effect can loop in the cyclic buffer.

### 12.4 The real-time budget, and how it is asserted

The refill mixes up to 8 voices over 1024 frames, ~47 times a second, with
interrupts off. That is a budget, and the honest way to check it is not to
assert in a comment that integer multiply-adds are cheap:

- `hda_pcm_underruns()` counts `FIFOE` — the controller reaching a slice the
  mixer had not finished writing. Under streaming that is the *only* failure a
  too-slow render can produce, so `tests/kernel/test_hda_pcm_k.c` requires it
  to stay 0 with every voice sounding.
- The "frame timing is unaffected" half of SCRUM-212's acceptance is checked as
  a **ratio between two independent clocks**, because measuring one against
  itself proves nothing: a test that timed its own wait with
  `kernel_get_ticks_ms()` would spin longer in wall time and still report the
  interval it asked for. Instead the PIT's millisecond count gates the wait and
  the stream's own DMA rate is the reference — refills are driven by the
  controller finishing frames at 48 kHz, which owes nothing to IRQ0, so a
  handler holding interrupts off long enough to lose timer ticks would inflate
  the refill count over a fixed number of PIT milliseconds.

### 12.5 What this is not

Ring 0 only, with no ownership binding — the `src/disk_binding.c` equivalent
for HDA does not exist, and neither does `exo_sound_pcm`. Both are separate,
explicitly-blocked tickets. Nothing in ring 3 can reach the mixer, and Doom's
own sound module is still SCRUM-101's one-voice PC speaker sequencer
(`src/doom_sound.c`); rewiring that to real PCM waits on the syscall.
