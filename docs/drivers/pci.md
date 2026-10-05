# Driver: PCI bus enumeration and configuration space

**Files:** `src/pci.c`, `src/pci.h` **Status:** ✅ Complete (SCRUM-209)
**Last updated:** 25 Sep 2026

---

## Table of Contents

1. [Purpose](#1-purpose)
2. [Configuration space access](#2-configuration-space-access)
3. [Enumeration](#3-enumeration)
4. [Base Address Registers](#4-base-address-registers)
5. [Enabling a device](#5-enabling-a-device)
6. [API reference](#6-api-reference)
7. [Testing](#7-testing)
8. [Design decisions and gotchas](#8-design-decisions-and-gotchas)

---

## 1. Purpose

Before SCRUM-209 nothing in `src/` walked the PCI bus — every device the
kernel spoke to (COM1, the PIT, the PS/2 controller, the primary ATA bus)
lives at a fixed legacy I/O port and needs no discovery. Anything newer does:
its registers are wherever firmware decided to put them, and the only way to
find out is config space.

This driver is the prerequisite for the Audio epic's real-hardware half — the
Intel HDA controller (class `0x0403`) is found, sized and enabled through
here — and for any other PCI device this kernel ever wants.

Ring 0 only, and deliberately syscall-free, the same relationship
`src/pic.c`/`src/pit.c`/`src/ata.c` have to their hardware. It has no notion
of a caller or an owner; ring 3 cannot reach ports `0xCF8`/`0xCFC` itself
(IOPL 0, no I/O permission bitmap — see `tests/kernel/test_port_io_fault_k.c`).

## 2. Configuration space access

Legacy **configuration mechanism #1**: two 32-bit I/O ports.

| Port     | Name             | Role                                          |
| -------- | ---------------- | --------------------------------------------- |
| `0xCF8`  | `CONFIG_ADDRESS` | Which bus/device/function/register to address  |
| `0xCFC`  | `CONFIG_DATA`    | Reads and writes that register                |

`CONFIG_ADDRESS` layout:

| Bit 31 | Bits 30–24 | Bits 23–16 | Bits 15–11 | Bits 10–8 | Bits 7–0        |
| ------ | ---------- | ---------- | ---------- | --------- | --------------- |
| Enable | Reserved   | Bus        | Device     | Function  | Register offset |

The register offset's low two bits are always zero — every access through
this port pair is a whole aligned dword. `pci_config_read16`/`_read8` do that
dword access and shift/mask in software; `pci_config_write16` is a
read-modify-write of the containing dword for the same reason (the byte
enables are not reachable from software here).

**A function that does not exist answers anyway.** The host bridge completes
the access without error and returns all ones. Since there is no vendor ID
`0xFFFF`, `vendor == 0xFFFF` *is* the probe — see
`test_absent_function_reads_all_ones`.

Not implemented, and why:

- **Mechanism #2** — deprecated in PCI 2.0 (1993), not emulated by QEMU.
- **ECAM/MMCONFIG** — the PCIe memory-mapped mechanism. It is strictly
  better (it reaches the extended config space above offset `0xFF` that
  mechanism #1 cannot), but its base address comes from the ACPI `MCFG`
  table, and this kernel parses no ACPI at all. Mechanism #1 is mandatory on
  every machine that supports ECAM, so nothing is lost today; ECAM can be
  layered behind the same `pci_config_*()` API later without touching a
  caller.

## 3. Enumeration

`pci_init()` runs the **recursive scan**, not the brute-force one: bus 0 is
walked, and a PCI-to-PCI bridge (class `0x06`, subclass `0x04`) found on it
is followed to the secondary bus number in its own config space. Brute force
would mean 8192 `checkDevice()` calls over buses that mostly do not exist.

Three details the shape depends on:

- **Function 0's multi-function bit gates functions 1–7.** A single-function
  device answers *every* function number with function 0's registers, so
  scanning all eight unconditionally would record the same device eight
  times. `test_no_duplicate_functions_recorded` is the guard.
- **Functions need not be contiguous.** A device may implement function 0, 1
  and 7; the loop skips a `0xFFFF` vendor rather than stopping at it.
- **A multi-function host bridge at 0:0.0 means one host controller per
  function**, each responsible for the bus with its own function number, so
  those buses are scanned directly instead of waiting to be reached through
  a bridge.

Bus numbers are **read, never assigned**. Configuring bridges ourselves would
mean owning bus-number allocation *and* the memory/prefetch/IO window
forwarding for every bridge in the tree — and on the one platform this
targets, firmware has already done it before GRUB hands off. A visited-bus
bitmap (256 bits, 4 words) makes the recursion terminate regardless: a bridge
reporting a secondary bus it is itself behind must not hang boot.

Results land in a fixed 32-entry table. It is fixed-size because enumeration
runs early and should not depend on an allocator; overflow is reported on
serial and counted by `pci_devices_skipped()`, never silently dropped.

`pci_init()` is idempotent — calling it again re-scans from scratch, which is
what lets the test suite drive it without caring whether `kernel_main` got
there first.

On QEMU's default machine plus `-device intel-hda`, a boot prints:

```
pci: 7 function(s)
pci:   00:00.0  8086:1237  class 0600 prog_if 00 hdr 00
pci:   00:01.0  8086:7000  class 0601 prog_if 00 hdr 00
pci:   00:01.1  8086:7010  class 0101 prog_if 80 hdr 00
pci:   00:01.3  8086:7113  class 0680 prog_if 00 hdr 00
pci:   00:02.0  1234:1111  class 0300 prog_if 00 hdr 00
pci:   00:03.0  8086:100e  class 0200 prog_if 00 hdr 00
pci:   00:04.0  8086:2668  class 0403 prog_if 00 hdr 00
```

The last line is the acceptance case: the Intel HDA controller, class
`0x0403`.

## 4. Base Address Registers

`pci_bar_read()` decodes and **sizes** one BAR of a header-type-`0x0`
function:

| BAR bit 0 | Meaning                                          |
| --------- | ------------------------------------------------ |
| 1         | I/O space BAR — base is `BAR & 0xFFFFFFFC`        |
| 0         | Memory space BAR — base is `BAR & 0xFFFFFFF0`     |

For a memory BAR, bits 2–1 give the type (`0x2` = 64-bit, consuming *two*
BAR slots) and bit 3 is the prefetchable flag.

Sizing is the standard write-all-ones/read-back/restore sequence: the
writable address bits come back as the size mask, and the size is
`~mask + 1`. Two things make it more than three lines:

- **Decode goes off for the whole sequence.** The PCI spec warns that some
  devices decode the all-ones write as an unintended access, so
  `pci_bar_read()` clears the command register's I/O and memory space bits
  before touching the BAR and restores the register exactly as it was
  afterwards — restore of the BAR value included, while decode is still off.
  `test_bar_sizing_restores_the_original_value` reads the raw BAR *and* the
  raw command register before and after to prove it.
- **The unwritable half carries no information.** A 32-bit memory BAR's
  upper 32 bits and an I/O BAR's upper 16 are forced to all ones in the mask
  before inverting, or the computed size would be nonsense.

A readback with no writable bits at all means the BAR is not implemented:
`PCI_ENODEV`, with `size` set to 0.

**`pci_bar_assign()` is the unhappy path.** On QEMU — and on any BIOS or
UEFI machine — firmware programs every BAR before GRUB runs, so
`pci_bar_read()` already reports a usable base and nothing calls this. It
exists for a BAR firmware left at zero, and hands out naturally-aligned
windows from a fixed MMIO range:

```
PCI_MMIO_WINDOW_BASE  0xE0000000
PCI_MMIO_WINDOW_END   0xF0000000
```

That range sits in the 32-bit PCI hole QEMU leaves between the top of low
RAM and the LAPIC/IOAPIC block at `0xFEC00000`, above any RAM this kernel is
booted with and clear of the framebuffer aperture QEMU places at
`0xFD000000`. It is a checked-in constant rather than something derived at
runtime because there is no ACPI `_CRS` to ask — and a fixed window in the
source is auditable where a guessed one would not be.

**`pci_bar_map()` is what makes a base "usable".** `vmm_init()` identity-maps
low memory, the kernel image, usable RAM, the multiboot info and the
framebuffer aperture — and nothing else, so every other BAR lands on
unmapped memory. `pci_bar_map()` identity-maps the BAR's window
page-rounded with `VMM_PCD` (uncacheable: MMIO registers have read side
effects and must not be cached).

## 5. Enabling a device

`pci_enable_device()` sets, in the command register:

| Bit | Name         | When                                           |
| --- | ------------ | ---------------------------------------------- |
| 2   | Bus master   | Always — a device that cannot master the bus cannot DMA |
| 1   | Memory space | The function has at least one memory BAR        |
| 0   | I/O space    | The function has at least one I/O BAR           |

It only ever *sets* bits; whatever firmware had on stays on
(`test_enable_device_sets_decoders` asserts `after & before == before`).

## 6. API reference

| Function | Returns |
| --- | --- |
| `pci_config_read32/16/8(bus, dev, fn, off)` | Register value; all ones for an absent function |
| `pci_config_write32/16(bus, dev, fn, off, val)` | — |
| `pci_init(void)` | — (cannot fail; a machine with no PCI enumerates zero devices) |
| `pci_device_count(void)` / `pci_devices_skipped(void)` | Table occupancy / overflow count |
| `pci_device_at(idx)` | `const pci_device_t *`, `NULL` out of range |
| `pci_find_class(class, subclass)` / `pci_find_id(vendor, device)` | First match, or `NULL` |
| `pci_bar_read(dev, index, out)` | `PCI_OK` / `PCI_EINVAL` / `PCI_ENODEV` |
| `pci_bar_assign(dev, index, out)` | `PCI_OK` / `PCI_EINVAL` / `PCI_ENODEV` / `PCI_ENOSPC` |
| `pci_enable_device(dev)` | `PCI_OK` / `PCI_EINVAL` |
| `pci_bar_map(bar)` | `PCI_OK` / `PCI_EINVAL` / `PCI_ENOSPC` |
| `pci_dump(void)` | — (one serial line per function) |

Pointers returned by `pci_device_at`/`pci_find_*` point into the driver's
static table and stay valid until the next `pci_init()`.

## 7. Testing

`tests/kernel/test_pci_k.c`, suite `"pci"` — 12 tests. There is no mock:
config space is a pair of I/O ports, and a model of the host bridge would
have proved nothing about the one thing this ticket is for.

`make docker-test` / `make docker-ci` pass **`-device intel-hda`** to QEMU
(see the `Makefile`), so the audio-controller assertions are unconditional —
a build that stops finding class `0x0403`, or stops resolving its BAR0 to a
mapped MMIO base, fails CI. Every other assertion holds on any QEMU machine,
which always has a host bridge, an ISA bridge and an IDE controller.

## 8. Design decisions and gotchas

- **`inl` was added to `src/io.h` for this.** The header had `outl` (for
  `qemu_exit`) but no 32-bit read; `CONFIG_DATA` needs one.
- **Enumeration runs after `vmm_init()` in `kernel_main`.** Config-space
  access is pure port I/O and would work anywhere after `serial_init()`, but
  `pci_bar_map()` edits the kernel map `vmm_init()` installs. Like every
  other driver a KUnit suite drives, it sits ahead of the `TESTING` branch.
- **No IRQ handling *here*.** The `Interrupt Line` field (offset `0x3C`) is
  read/write and purely informational as far as this file is concerned: it is
  a place firmware leaves a note saying which 8259 line it routed the
  function's INTx pin to. Enumeration neither reads nor needs it, which is
  where SCRUM-209 stopped. `PCI_OFF_INTERRUPT_LINE`/`_PIN` were added to
  `src/pci.h` by SCRUM-210, and `src/hda.c` is the first driver to use them:
  it reads the line, installs its own stub on vector `32 + line`, and unmasks
  it at the PIC (plus the cascade for a line on the slave). That is legacy
  INTx only — the I/O APIC (which needs MP or ACPI table parsing) and MSI are
  both still unbuilt, and see `docs/drivers/hda.md` §7 for why one vector per
  driver means a device routed onto IRQ 0/1/2 is refused rather than wired.
- **No capability-list walk.** MSI/MSI-X live in the capabilities linked
  list at offset `0x34`; a driver that wants them can walk it through
  `pci_config_read8()` without this file growing an opinion. Nothing does yet
  — SCRUM-210's HDA driver takes its interrupt through INTx precisely because
  the PIC and IDT already exist and MSI would have to be built first.
- **256 bytes per function, not 4096.** Mechanism #1's hard limit — see §2.
