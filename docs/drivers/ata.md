# Driver: ATA (PIO, primary bus, master drive)

**Files:** `src/ata.c`, `src/ata.h` **Status:** ✅ Complete (SCRUM-102)
**Last updated:** 22 Sep 2026

---

## Table of Contents

1. [Purpose](#1-purpose)
2. [Hardware background](#2-hardware-background)
3. [Detection (IDENTIFY)](#3-detection-identify)
4. [Sector read/write](#4-sector-readwrite)
5. [API reference](#5-api-reference)
6. [Testing](#6-testing)
7. [Design decisions and gotchas](#7-design-decisions-and-gotchas)

---

## 1. Purpose

`ata.c` is the kernel's only path to persistent block storage: detect a
drive and read/write individual 512-byte sectors by LBA. It has no
filesystem knowledge and no syscall surface — those are separate, later
tickets:

- `exo_disk_read`/`exo_disk_write` (SCRUM-103) put a bounds-checked syscall
  boundary in front of this driver, the same way `exo_page_alloc` sits in
  front of `page_alloc.c`.
- A disk ownership/binding table (SCRUM-188) mirrors `fb_binding.c` on top
  of those syscalls.
- A FAT-like filesystem (SCRUM-189) is ported entirely in LibOS space, on
  top of the syscalls — it never touches `ata.c` directly.

---

## 2. Hardware background

QEMU's `-drive if=ide` attaches to the **primary ATA bus** as the **master**
drive — confirmed as the chosen interface on SCRUM-102 (2026-09-19), which
rejected a SCSI/virtio-scsi HBA as unnecessary driver complexity for a
QEMU-only demo. This driver only ever speaks to that one drive: no slave, no
secondary bus.

| Port     | Register (read)  | Register (write)  |
| -------- | ----------------- | ------------------ |
| `0x1F0`  | Data               | Data                |
| `0x1F1`  | Error              | Features             |
| `0x1F2`  | Sector count       | Sector count         |
| `0x1F3`  | LBA low (bits 0-7) | LBA low              |
| `0x1F4`  | LBA mid (bits 8-15)| LBA mid              |
| `0x1F5`  | LBA high (bits 16-23)| LBA high           |
| `0x1F6`  | Drive/head         | Drive/head           |
| `0x1F7`  | Status             | Command               |

Drive/head register for a 28-bit LBA command: `0xE0 | ((lba >> 24) & 0xF)` —
the `0xE0` selects LBA addressing mode and the primary/master drive (bit 4
clear); the low nibble carries LBA bits 24-27.

Status register bits this driver checks:

```
7   6   5   4   3   2   1   0
BSY DRDY DF  ―  DRQ  ―  ―  ERR
```

`BSY` (busy) must clear before any other status bit is meaningful. `DRQ`
(data request) means the drive is ready to transfer a word through the data
port. `ERR`/`DF` (error / drive fault) mean the command failed.

---

## 3. Detection (`ata_init`)

Sends `IDENTIFY DEVICE` (command `0xEC`) to the primary master with a
zeroed LBA/sector-count, then:

1. Reads status immediately. `0x00` means the bus is floating — no drive
   wired at all — and `ata_init()` returns `ATA_ENODEV` rather than
   treating it as an error to propagate loudly. This mirrors `DG_Init`'s
   "report, don't halt" convention for a missing resource
   (`docs/architecture.md` §7, `src/doom_wad.c`).
2. Polls until `BSY` clears (`ata_wait_not_busy`).
3. Polls until `DRQ` sets or `ERR`/`DF` sets (`ata_wait_drq`).
4. Drains the 256-word IDENTIFY payload. Detection only needs proof that a
   full response arrived intact — nothing here parses the model string,
   serial number, or capacity fields out of it. A later ticket can extend
   this if that data becomes useful (e.g. reporting real drive geometry).

Callers must call `ata_init()` once before `ata_read_sector`/
`ata_write_sector`; behaviour before that is an unbounded hardware timeout,
not a crash, but is still undefined in the sense that no drive has been
selected into a known state yet.

---

## 4. Sector read/write

Both `ata_read_sector(lba, buf)` and `ata_write_sector(lba, buf)`:

1. Program drive/head (LBA mode, master, top 4 LBA bits), sector count = 1,
   and the low/mid/high LBA bytes (`ata_select_sector`).
2. Issue `READ SECTORS` (`0x20`) or `WRITE SECTORS` (`0x30`).
3. Poll `BSY` clear, then `DRQ` set (identical polling helpers to
   `ata_init`).
4. Transfer 256 words (512 bytes) through the data port — `inw`/`outw` in a
   loop, not a `rep insw`/`outsw` string instruction; this project's `io.h`
   already establishes plain `inb`/`outb` wrappers as the house style for
   every port-I/O driver (`pic.c`, `pit.c`, `ps2.c`), and a 256-iteration
   loop costs nothing that matters against QEMU's timing.
5. **Write only:** issues `CACHE FLUSH` (`0xE7`) and waits for `BSY` to
   clear again before returning `ATA_OK`. Without this, a read of the same
   LBA immediately after a write is racing QEMU's own write-back timing
   rather than observing a guaranteed-durable sector — exactly what the
   round-trip test in §6 depends on.

---

## 5. API reference

```c
#define ATA_OK        0
#define ATA_ENODEV  (-1)   /* no drive present (status read as 0x00) */
#define ATA_ETIMEOUT (-2)  /* BSY never cleared / DRQ never set      */
#define ATA_EIO     (-3)   /* ERR bit set in the status register     */

int ata_init(void);
int ata_read_sector(uint32_t lba, uint8_t *buf);
int ata_write_sector(uint32_t lba, const uint8_t *buf);
```

`buf` must point to at least 512 bytes. LBA is 28-bit (`lba` fits in the
low 28 bits of a `uint32_t`); 48-bit LBA addressing is not implemented —
not needed for the small FAT image SCRUM-190 will attach.

---

## 6. Testing

`tests/kernel/test_ata_k.c` (suite `ata`, registered in
`tests/kernel/test_runner.c`) drives the real driver against whatever is
attached to the primary bus:

- `ata_init` returns `ATA_OK` or `ATA_ENODEV` (never a timeout/IO error) —
  true whether or not a drive is actually attached.
- A write/read round trip at LBA 2048 — chosen far from LBA 0 so a future
  FAT superblock reusing this same scratch image (SCRUM-189/190) is never
  at risk — only runs its assertions when a drive was detected, since
  `docker-run`/`docker-run-kernel` attach none.
- A read of an untouched sector (LBA 2049) returns `ATA_OK` with no fault,
  as a cheap regression check on the polling loop's exit condition.

**`docker-test`/`docker-ci` only** attach a scratch drive: the `Makefile`
creates an 8 MB zeroed `build/ata_scratch.img` and passes
`-drive file=...,format=raw,if=ide` to `qemu-system-x86_64`. This is
throwaway test infrastructure, not the real FAT-formatted image —
`docker-run`/`docker-run-kernel`/`docker-build` are unchanged and still
boot with no disk attached, so `ata_init()` reports `ATA_ENODEV` there
(logged on serial, boot continues — see `src/kernel.c`).

---

## 7. Design decisions and gotchas

**Polled, not IRQ14-driven.** "PIO" here means programmed I/O with a
busy-polled status register. An interrupt-driven driver would need a new
IDT vector/ISR stub for IRQ14, which the ticket's acceptance bar (detect a
drive, round-trip a sector) doesn't require. If a future ticket needs
non-blocking disk I/O, this is the place that would grow an IRQ path.

**Bounded iteration count, not a PIT-timed timeout.** `ata_wait_not_busy`/
`ata_wait_drq` spin for up to `ATA_POLL_ITERS` (100,000) loop iterations
rather than consulting `kernel_get_ticks_ms()`. BSY/DRQ resolve in
microseconds on real hardware and in QEMU; tying this to the PIT would add
a cross-driver dependency (`ata.c` → `pit.c`) for a wait that's expected to
be essentially instantaneous. A genuinely wedged drive still eventually
returns `ATA_ETIMEOUT` rather than hanging boot forever.

**`ata_init()` reads status `0x00` as "no drive," not an error.** A floating
bus (nothing wired to the primary IDE port at all) reads back all zero
bits on every register, `BSY` included — waiting for `BSY` to clear here
would spin the full iteration budget on hardware that was never going to
answer. Checking status immediately after issuing `IDENTIFY`, before the
first poll loop, catches this cheaply and lets `ata_init()` return
`ATA_ENODEV` promptly instead of `ATA_ETIMEOUT` after a full spin.

**No slave/secondary-bus support.** The public API has no drive-select
parameter by design — SCRUM-102's scope is exactly "detect drive, read/
write 512-byte sectors" against the one drive QEMU's `-drive if=ide`
attaches. Multi-drive support is not needed anywhere in the current
roadmap (`docs/architecture.md` §10) and would be a straightforward
extension (a drive-select byte threaded through the existing register
layout) if that changes.
