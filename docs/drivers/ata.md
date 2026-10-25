# Driver: ATA (PIO, both legacy buses, master and slave)

**Files:** `src/ata.c`, `src/ata.h` (registers into `src/blkdev.c/h`)
**Status:** ✅ Complete (SCRUM-102, multi-sector PIO SCRUM-190, four
positions + block-device registration SCRUM-244)
**Last updated:** 24 Oct 2026

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

`ata.c` is the kernel's path to IDE block storage: detect drives and
read/write 512-byte sectors by LBA. It has no filesystem knowledge and no
syscall surface. Since SCRUM-244 it also has no direct callers above it: it
registers each disk it finds in the kernel block-device registry
(`src/blkdev.h`), and the syscall layer reaches a disk only through that
registry by device index (`docs/syscall_spec.md` §3.5a). The layers on top:

- `exo_disk_read`/`exo_disk_write` (SCRUM-103) put a bounds-checked syscall
  boundary in front of this driver, the same way `exo_page_alloc` sits in
  front of `page_alloc.c`.
- A disk ownership/binding table (SCRUM-188) mirrors `fb_binding.c` on top
  of those syscalls.
- A FAT-like filesystem (SCRUM-189) is ported entirely in LibOS space, on
  top of the syscalls — it never touches `ata.c` directly.

---

## 2. Hardware background

QEMU's `-drive if=ide` attaches to the legacy IDE buses — confirmed as the
chosen interface on SCRUM-102 (2026-09-19), which rejected a SCSI/virtio-scsi
HBA as unnecessary driver complexity for a QEMU-only demo. There are four
positions, which QEMU numbers with `index=`:

| Position (`index=`) | Bus       | Drive  | Command block | Control | Test boot attaches |
| ------------------- | --------- | ------ | ------------- | ------- | ------------------ |
| 0                   | primary   | master | `0x1F0`–`0x1F7` | `0x3F6` | `build/exodoom_disk.img` — device 0, the system disk |
| 1                   | primary   | slave  | `0x1F0`–`0x1F7` | `0x3F6` | `build/exodoom_disk2.img` — device 1 (SCRUM-244) |
| 2                   | secondary | master | `0x170`–`0x177` | `0x376` | the `-cdrom` ISO (ATAPI, skipped) |
| 3                   | secondary | slave  | `0x170`–`0x177` | `0x376` | nothing |

The `Makefile` pins the system disk to `index=0` on every QEMU line so the
two drives cannot swap. Registers below are at offsets from the bus's
command-block base (primary shown):

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

Drive/head register for a 28-bit LBA command: `0xE0 | ((lba >> 24) & 0xF)`
for the master, `0xF0 | …` for the slave — `0xE0` selects LBA addressing
mode, bit 4 (`0x10`) selects the slave; the low nibble carries LBA bits
24-27. Master and slave share a bus's registers, so every command rewrites
drive/head with its own drive's bit and then waits ~400 ns before polling,
since the previous command may have been for the other drive.

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

`ata_init()` probes all four positions in order (`ata_probe()`). For each it
selects the drive, waits ~400 ns, and:

1. Reads status. `0x00` (QEMU's answer for an absent slave or an empty bus)
   or `0xFF` (a floating bus on real hardware) means nothing is there, and
   the position is marked absent — "report, don't halt", `DG_Init`'s
   convention for a missing resource (`docs/architecture.md` §7).
2. Sends `IDENTIFY DEVICE` (`0xEC`) with a zeroed LBA/sector-count, and
   reads status again (`0x00` → absent).
3. Polls until `BSY` clears (`ata_wait_not_busy`).
4. **Checks for an ATAPI signature** (LBA mid/high = `0x14`/`0xEB`). A packet
   device — the CD-ROM GRUB booted from, at QEMU's secondary master —
   aborts `IDENTIFY DEVICE` and leaves that signature. It is checked
   *before* waiting for DRQ, which an aborted command never raises; waiting
   would only find `ERR` and make the CD-ROM look like a faulted disk rather
   than not-a-disk. It is marked absent.
5. Polls until `DRQ` sets or `ERR`/`DF` sets (`ata_wait_drq`).
6. Drains the 256-word IDENTIFY payload, keeping word 47 (multiple-mode
   block, §4) and words 60/61 (28-bit LBA sector count, reported by #31).

The return value is the **primary master's** result (`ATA_OK`, `ATA_ENODEV`,
or a timeout/IO error), so callers that ask "is the system disk there?" are
unchanged; `ata_drive_present(pos)`/`ata_drive_sector_count(pos)` answer for
any position. `ata_init()` is safe to call again — the KUnit suites do — and
only re-probes; it registers nothing.

**Registration.** `kernel_main` calls `ata_register_blkdevs()` once, after
`ata_init()`. The primary master goes in the registry's reserved system slot
with `blkdev_register_system()` — block device 0 — and every other present
position takes the next free slot from 1 up with `blkdev_register()`, in
position order; the primary slave is device 1 on the test boot. With no
primary master, device 0 stays empty instead of going to whichever disk was
found next (`docs/syscall_spec.md` §3.5a has why). `syscall_disk_init()`
follows, after `usb_init()`.

**Empty positions are rejected without a long wait.** After the status
check, the probe writes `0x55`/`0xAA` to the sector-count and LBA-low
registers and reads them back. A drive that is there latches them; an empty
position does not (QEMU reads an absent drive's registers as `0`, a floating
bus reads `0xFF`). Without this, a floating bus that settles at something
like `0x7F` or `0x80` — BSY apparently set — would make the BSY poll spin its
whole `ATA_POLL_ITERS` budget (~10 s) for each empty position.

---

## 4. Sector read/write

`ata_read_sector(lba, buf)`/`ata_write_sector(lba, buf)` are thin,
count-1 wrappers around `ata_read_sectors(lba, buf, count)`/
`ata_write_sectors(lba, buf, count)` (SCRUM-190), which address the primary
master; every other drive is reached only through the block-device ops
`ata_register_blkdevs()` registers, which call the same code with the drive
bound in. All of them do:

1. Program drive/head (LBA mode, this drive's master/slave bit, top 4 LBA
   bits) — waiting ~400 ns for the selection to settle only if that changed
   which drive the bus has selected (`ata_select_drive()`; each of the four
   alternate-status reads in the delay is a VM exit under KVM, so it is not
   paid on every command to the drive already selected) — then sector count and
   the low/mid/high LBA bytes (`ata_select`) — **once**, for the whole run,
   not once per sector. Re-issuing the command block per sector would reset
   the drive's internal auto-incrementing LBA, which is exactly what
   multi-sector PIO exists to avoid paying for.
2. Issue `READ MULTIPLE` (`0xC4`) / `WRITE MULTIPLE` (`0xC5`) once — or
   `READ SECTORS` (`0x20`) / `WRITE SECTORS` (`0x30`) if the drive has no
   multiple mode (see below).
3. For each DRQ *block* of `ata_multiple_block()` sectors (the last one
   short if `count` is not a multiple of it): poll `BSY` clear, then `DRQ`
   set (identical polling helpers to `ata_init`), then transfer the whole
   block through the data port as one 32-bit `rep insl`/`rep outsl`
   (`insl()`/`outsl()` in `io.h`; see **32-bit PIO** below), **not** an
   `inw`/`outw` loop. The loop
   was the original choice on the theory that 256 iterations cost nothing
   against QEMU's timing — true under TCG, false under `-enable-kvm`, where
   every standalone `IN`/`OUT` is a full VM exit to QEMU's device model
   while a `REP` string instruction is batched into about one exit per
   page. For SCRUM-190's 28 MB WAD load that was ~14M exits and **~3
   minutes with interrupts off** (it looked like a hang). CI runs under TCG
   and cannot see this difference.

**Multiple mode.** `ata_init()` reads IDENTIFY word 47, per drive, (bits 7:0, the most
sectors the drive moves per DRQ block) and issues `SET MULTIPLE MODE`
(`0xC6`) with it; QEMU reports and accepts 16. With plain `READ SECTORS`
the drive raises DRQ once per 512-byte sector, and each of those
handshakes — a BSY poll, a DRQ poll and the `rep insw` — was ~5 VM exits
per sector, measured with QEMU's `info stats vcpu` over the WAD load. A
16-sector block pays that once per 8 KiB. Measured Enter→`W_Init` for
`doom` under KVM: **~6.3 s → ~2.7 s**, `io_exits` ~253K → ~59K. A drive
that reports 0/1 or rejects the command leaves the block at 1 and the
driver on the single-sector commands: slower, never wrong. §6's
`multiple mode negotiated` test fails if QEMU's drive ever lands there, so
a silent fallback cannot quietly give the time back.

**32-bit PIO.** The data register and the PATA cable are 16 bits wide;
a 32-bit `in` at `0x1F0` works only because the host controller splits it
into two 16-bit cable transfers. PCI IDE controllers such as the PIIX
QEMU emulates do; older ISA/VLB controllers did not, some chipsets needed
enabling first, and nothing reliably reports it (IDENTIFY word 48 is
obsolete and described the drive, not the controller), which is why Linux
leaves it off unless `hdparm -c` asks. Where it is unsupported the result
is garbage, not an error. It is used anyway because this driver is
QEMU-only in practice: real boards have no PATA and increasingly no IDE
compatibility mode, and SCRUM-242 (AHCI) is the real-hardware disk path.
IDENTIFY in `ata_init()` still uses 16-bit `insw()`, so probing a drive
assumes nothing beyond the spec. Measured Enter→`W_Init` under KVM:
**~2.7 s → ~0.92 s**.

**What is left is the PIO floor.** (Profiled before the 32-bit change,
which halved the access count without changing its shape.) `perf` on QEMU during the remaining
~2.7 s: ~75% of it is the vCPU thread in QEMU *userspace*, and the profile
is entirely the per-access port-I/O dispatch
(`kvm_cpu_exec → address_space_rw → flatview_read → portio_read →
ide_data_readw`, ~40% of it a mutex lock/unlock per access). KVM hands
QEMU a whole `rep insw` per exit, but QEMU still dispatches it one 16-bit
word at a time, ~14M times for the WAD. Block I/O does not show up at all.
Only bus-master DMA, which QEMU services as one block request straight into
guest memory, gets under this: SCRUM-241.

4. **Write only:** issues `CACHE FLUSH` (`0xE7`) and waits for `BSY` to
   clear again before returning `ATA_OK`. Without this, a read of the same
   LBAs immediately after a write is racing QEMU's own write-back timing
   rather than observing guaranteed-durable sectors — exactly what the
   round-trip test in §6 depends on.

**Why this exists.** SCRUM-190's WAD-on-disk load used to walk ExoFS's file
read path one 512-byte block at a time, each costing one `exo_disk_read`
syscall and (before this) one freshly-issued single-sector `READ SECTORS`
command — ~57,000 of each for a 28 MB file, observed as a multi-second
stall on the first `doom`/`wadview` launch. `exofs_file.c`'s `exofs_read()`
now batches contiguous on-disk blocks into one `exofs_bdev_read()` call
(src/libos_fs's own docs), and this driver is the matching half: one
multi-sector PIO command actually amortizes the per-command overhead
instead of just the syscall overhead on top of it. The 256-word-per-sector
data transfer itself is still PIO — there is no DMA here — but see step 3:
under KVM it must be a string instruction or it dominates everything else.

---

## 5. API reference

```c
#define ATA_OK        0
#define ATA_ENODEV  (-1)   /* no drive present (status read as 0x00) */
#define ATA_ETIMEOUT (-2)  /* BSY never cleared / DRQ never set      */
#define ATA_EIO     (-3)   /* ERR bit set in the status register     */

#define ATA_MAX_DRIVES 4   /* primary/secondary x master/slave         */

int ata_init(void);                       /* probe all; primary master's rc */
int ata_register_blkdevs(void);           /* once, from kernel_main         */
int ata_drive_present(unsigned drive);
uint32_t ata_drive_sector_count(unsigned drive);

/* Primary master (the system disk). */
int ata_read_sector(uint32_t lba, uint8_t *buf);
int ata_write_sector(uint32_t lba, const uint8_t *buf);
int ata_read_sectors(uint32_t lba, uint8_t *buf, uint32_t count);
int ata_write_sectors(uint32_t lba, const uint8_t *buf, uint32_t count);
uint32_t ata_get_sector_count(void);
uint32_t ata_multiple_block(void);
```

`buf` must point to at least `count` (1 for the single-sector calls) * 512
bytes. LBA is 28-bit (`lba` fits in the low 28 bits of a `uint32_t`); 48-bit
LBA addressing is not implemented — not needed for the FAT-formatted image
SCRUM-190 attaches. `count` must be in `[1, 255]` (0 would select 256
sectors per the ATA spec's own encoding of the sector-count register, which
nothing here needs); `count == 0` returns `ATA_OK` without touching
hardware rather than being rejected, as a defensive no-op symmetrical with
every other layer above it (`exofs_bdev_read`/`_write`,
`src/syscall_disk.c`) treating it the same way, even though the one caller
below never actually reaches here with 0.

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
- `multiple mode negotiated`: `ata_multiple_block() > 1` once a drive is
  present (QEMU accepts 16) — see §4's multiple-mode note for why a silent
  fallback to 1 is worth failing on.
- A 37-sector `ata_write_sectors`/`ata_read_sectors` round trip at LBA
  2112 (16 + 16 + 5 with QEMU's block), so the short final DRQ block is
  exercised in both directions, plus a single-sector read of the last one
  to prove the run landed at the right LBAs.

Every QEMU line in the `Makefile` attaches `build/exodoom_disk.img` at
`index=0` and the blank `build/exodoom_disk2.img` (8 MiB, created by
`docker/scripts/build.sh` when missing) at `index=1` (SCRUM-244). A boot
without a disk still soft-fails: `ata_init()` reports `ATA_ENODEV` (logged on
serial, boot continues — see `src/kernel.c`).

`tests/kernel/test_blkdev_k.c` (suite `blkdev`) checks registration against
probing: one block device per present position, in position order; the
primary master is device 0; the ATAPI CD-ROM at position 2 is **not**
registered. `tests/kernel/test_multidisk_k.c` drives the second drive
through the syscalls (`docs/syscall_spec.md` §3.5a).

---

## 7. Design decisions and gotchas

**Polled, not IRQ14-driven.** "PIO" here means programmed I/O with a
busy-polled status register. An interrupt-driven driver would need a new
IDT vector/ISR stub for IRQ14, which the ticket's acceptance bar (detect a
drive, round-trip a sector) doesn't require. If a future ticket needs
non-blocking disk I/O, this is the place that would grow an IRQ path.

**Bounded iteration count, not a PIT-timed timeout.** `ata_wait_not_busy`/
`ata_wait_drq` spin for up to `ATA_POLL_ITERS` (10,000,000) loop iterations
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

**Four positions, one code path (SCRUM-244).** SCRUM-102 deliberately spoke
to the primary master only. `install` (SCRUM-233) needs a second disk, so
the port base, control port and drive-select bit became per-drive state
(`ata_drive_t`) and every helper takes the drive. The unsuffixed calls kept
their signatures as primary-master wrappers so ring-0 users of the system
disk did not change.

**Device 0 is the primary master or nothing.** Device numbers are not IDE
positions, but slot 0 is reserved for the primary master: with only a slave
attached, the slave is device 1 and device 0 is absent, so it can never be
mistaken for the system disk. Nothing above this file depends on any other
position-to-device mapping.

**The CD-ROM is skipped, not driven.** Packet (ATAPI) devices take
`PACKET` commands, not `READ SECTORS`, and this driver has no use for one,
so the probe identifies it by signature and leaves it alone.
