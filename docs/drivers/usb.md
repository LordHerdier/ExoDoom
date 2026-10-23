# Driver: USB host controllers (decision note)

**Files:** no driver yet; it arrives with SCRUM-198. Topology test:
`tests/kernel/test_usb_topology_k.c` (suite `usb_topology`).
**Status:** 📝 Design settled (SCRUM-245 spike); no code yet
**Last updated:** 23 Oct 2026

---

## Table of Contents

1. [Purpose](#1-purpose)
2. [Topology in QEMU](#2-topology-in-qemu)
3. [Interrupt delivery: polled from IRQ0](#3-interrupt-delivery-polled-from-irq0)
4. [Root ports: several UHCI controllers, no hub](#4-root-ports-several-uhci-controllers-no-hub)
5. [QEMU input routing](#5-qemu-input-routing)
6. [Constraints on the stories that follow](#6-constraints-on-the-stories-that-follow)
7. [Appendix: measurements](#7-appendix-measurements)

---

## 1. Purpose

Before SCRUM-245 nothing in `src/` or `docs/` touched USB. The USB epic
(SCRUM-243) layers a host controller driver, a small USB core, and class
drivers: HID keyboard SCRUM-198, HID mouse SCRUM-246, mass storage SCRUM-247.
Two decisions change the shape of the first of those, so this spike made them
from QEMU measurements before any driver code was written:

- how the controller's interrupt reaches the kernel, given there is no I/O
  APIC, no MSI and no shared-IRQ dispatch (`docs/drivers/pci.md` §8,
  `docs/drivers/hda.md` §7);
- how four devices fit on a controller type with two root ports.

Everything below the "decision" headings was measured on QEMU 10.0.13
(`exodoom-qemu` image, `pc`/i440fx machine, SeaBIOS). §7 has the raw data.

## 2. Topology in QEMU

Three UHCI controllers, giving six root ports, with every device on an
explicit port:

```
-device ich9-usb-uhci1,id=uhci1          # 00:05.0  8086:2934  IRQ 10
-device ich9-usb-uhci2,id=uhci2          # 00:06.0  8086:2935  IRQ 11 (shared with HDA)
-device ich9-usb-uhci3,id=uhci3          # 00:07.0  8086:2936  IRQ 10
-device secondary-vga,id=vga2            # input console for keyboard A (§5)
-device secondary-vga,id=vga3            # input console for keyboard B
-device usb-kbd,bus=uhci1.0,port=1,display=vga2
-device usb-kbd,bus=uhci1.0,port=2,display=vga3
-device usb-mouse,bus=uhci2.0,port=1     # test targets only until SCRUM-246 (§5)
-drive if=none,id=stick,file=...,format=raw
-device usb-storage,bus=uhci3.0,port=1,drive=stick
```

The IRQ and slot numbers are what SeaBIOS assigns with today's device list in
front. They move if a device is added before them. Nothing may hard-code them:
read `Interrupt Line` the way `hda.c` does.

- **Always pass `port=`.** When only one root port is left, QEMU attaches an
  automatic `usb-hub` there and puts the next device behind it. Two keyboards
  on one 2-port controller without `port=` came out as "Port 1 keyboard,
  Port 2 hub, Port 2.1 keyboard" (§7, run A). With no hub driver (§4), that
  second keyboard would be invisible.
- `ich9-usb-uhciN` works standalone on `pc`, with no EHCI companion. Several
  `piix3-usb-uhci` instances work just as well (run D). They differ only in
  device ID, and PIIX3's UHCI always signals on INTD.

## 3. Interrupt delivery: polled from IRQ0

**Decision:** the controllers run with `USBINTR = 0` and are polled from
`irq0_handler()` (`src/pit.c`), the same way `speaker_tick()` and `hda_tick()`
are serviced. The hook is a static-flag no-op until a controller is brought up.

**Why not wire the INTx line:** in every topology measured, at least one UHCI
function lands on IRQ 11 with `intel-hda` (§7). Today's command line already
puts HDA on 11 next to QEMU's default e1000, which nothing drives. Each
driver installs its own stub on `32 + line` (`docs/drivers/hda.md` §7), so a
second driver claiming 11 would silently replace HDA's handler. A controller
with `USBINTR = 0` never asserts INTx, so it can sit on any line, HDA's
included, without anyone needing to know.

**Why polling costs nothing that matters:**

- UHCI walks its schedule once per 1 ms frame, and the PIT ticks at 1 kHz
  (`pit_init(1000)`, `src/kernel.c`). A 1 ms poll adds at most one frame of
  latency.
- HID devices report at the interval in their endpoint descriptor's
  `bInterval`, which is several ms, so most ticks find nothing new.
- **Mass storage does not use interrupts at all.** Bulk-Only Transport is
  synchronous request/response, so SCRUM-247 sends a CBW, runs the data
  phase and reads the CSW, waiting on the TDs in the calling context, the way
  `src/ata.c` waits on the drive. The limit is UHCI's full-speed frame
  bandwidth (about 1 MB/s), not how fast completion is noticed. So the
  ticket's "switch to shared INTx if mass-storage throughput needs it" trigger
  does not fire for storage.

**Budget for the IRQ0 hook** (SCRUM-181 moved the compositor's memcpy out of
this ISR, and the same rule applies here). The hook may, per controller:

- read `USBSTS`,
- read the status dword of each active interrupt-IN TD,
- copy a completed HID report (≤ 8 bytes) into a ring,
- re-arm that TD (two dword writes).

It may not walk the whole schedule, run control or bulk transfers, or
enumerate. Hot-plug (`PORTSC` connect-status-change) is noted here and acted on
outside the ISR.

**When to revisit:** only if something needs sub-millisecond completion or
`hlt`-idle wakeups from USB. At that point, build a shared-INTx dispatcher
(one stub per line that calls every registered "was this mine?" check, which
also means changing `hda.c`). Pinning the IRQ with a QEMU option (the
ticket's option (c)) is rejected: it does nothing on real hardware.

## 4. Root ports: several UHCI controllers, no hub

**Decision:** drive every UHCI function that PCI reports, and do not write a
hub class driver.

- Match class `0x0C`, subclass `0x03`, prog-if `0x00`. `pci_find_class()`
  returns only the first match, so loop over `pci_device_at(0 ..
  pci_device_count() - 1)`. `PCI_MAX_DEVICES` (32) leaves room: the full
  topology in §2 is 12 functions.
- **Find the port count by probing, not by assuming two.** UHCI has no
  register for it. Port `n`'s `PORTSC` is at I/O BAR 4 + `0x10` + 2n, and bit
  7 is reserved and always reads 1 on a port that exists. Stop at the first
  port whose bit 7 is 0 or which reads `0xFFFF`, and cap at 7, as Linux
  `uhci-hcd`'s `uhci_count_ports()` does. QEMU's UHCI has 2 ports.
- **No hub driver**, because the only setting that would need one gains little
  from it. Boards from about 2015 on are xHCI-only (SCRUM-248), and xHCI has
  enough root ports for the demo setup. In QEMU, more controllers cost one
  `-device` each. Hub descriptors, hub port status and a second level of
  enumeration can be a follow-up if it is ever needed. Until then, §2's
  "always pass `port=`" rule stops QEMU from adding a hub on its own.

## 5. QEMU input routing

QEMU sends host keyboard events to the **most recently activated keyboard
handler bound to the console**. A `usb-kbd` is activated when it is created,
not when a guest driver claims it. Measured with HMP `sendkey` on the real
kernel (§7):

| Command line | Shell receives `help`? |
|---|---|
| PS/2 only | yes |
| `+ usb-kbd` | **no.** Every key goes to the undriven USB keyboard |
| `+ secondary-vga,id=vga2` and `usb-kbd,...,display=vga2` | yes |

So each `usb-kbd` is bound to a `secondary-vga` console of its own with
`display=`. Console 0 keeps going to the PS/2 keyboard, and the run targets
can carry the USB keyboards from now on. The extra displays don't disturb
the GRUB/VESA framebuffer: they enumerate as class `0x0380`, and the kernel
prints to the primary VGA unchanged.

This binding is also the likely way to type into the two dual-Doom keyboards
separately in QEMU (one console per keyboard, or QMP `input-send-event` with
`device` naming a console). That was not testable without SCRUM-198's driver.

`usb-mouse` has **no `display=` property**, so in a GUI display it would take
mouse events from the PS/2 mouse. It is therefore in the test command lines
only, until SCRUM-246 lands. `-display curses` (`docker-run*`) has no mouse
anyway.

## 6. Constraints on the stories that follow

- **SCRUM-198 (UHCI + enumeration + keyboard):**
  - Put the controller behind a small HCD interface: control, interrupt and
    bulk transfers, plus root-port status. SCRUM-248 then reuses the USB core
    and the class drivers unchanged.
  - **Take the controller from the firmware first.** SeaBIOS has already
    driven these keyboards (USB legacy support for its boot menu), so the
    devices arrive addressed and configured, and **the controllers are
    still running** when the kernel boots. Measured: `USBCMD = 0xC1` (run,
    configured, 64-byte max packet), bus mastering on, and the frame lists
    at `0x0FFDC000`–`0x0FFDE000`. Clear `LEGSUP` (PCI config `0xC0`) the way
    Linux does (write `0x8F00`), then do a global reset (`USBCMD.GRESET`) and
    a host-controller reset (`USBCMD.HCRESET`) before building a schedule.
  - Until that driver exists, those running controllers are safe only
    because every structure they touch sits in memory the e820 map marks
    reserved, which `page_alloc` never hands out (§7). The `usb_topology`
    suite walks the firmware's schedule and fails if that stops being true.
  - The frame list (4 KiB, 4 KiB-aligned) and every QH, TD and buffer must
    sit below 4 GB, because UHCI addresses are 32-bit. Use `alloc_page()`, and
    assert the address.
  - Bring-up goes before the `TESTING` branch of `kernel_main`, after
    `pci_init()` (CLAUDE.md "Boot flow"), so KUnit can reach it.
  - Polling per §3. No IDT gate and no PIC unmask.
- **SCRUM-246 (mouse):** add `usb-mouse` to the run targets only once the
  driver claims it, and accept that a GUI display then routes the host mouse
  to it rather than to PS/2.
- **SCRUM-247 (mass storage):** completion is waited for synchronously, as in
  §3. The `usb-storage` drive is attached on `uhci3` port 1.
- **SCRUM-248 (xHCI):** needs its own interrupt decision only if it wants
  MSI. INTx plus polling carries over as is.

## 7. Appendix: measurements

QEMU 10.0.13, `-m 256M`, today's `-drive ... if=ide` and
`intel-hda`/`hda-output` in front of every run. `info pci` / `info usb` come
from the HMP monitor after SeaBIOS POST.

**Interrupt Line per topology:**

| Run | USB devices | UHCI functions → IRQ | HDA |
|---|---|---|---|
| 0 | none | — | 11 (e1000 also 11) |
| A | 1× `piix3-usb-uhci` | 00:05 → 11 | 11 |
| B | `ich9-usb-uhci1..3` | 00:05 → 10, 00:06 → 11, 00:07 → 10 | 11 |
| C | 1× `piix3-usb-uhci` + `usb-hub` | 00:05 → 11 | 11 |
| D | 3× `piix3-usb-uhci` | 00:05 → 11, 00:06 → 10, 00:07 → 10 | 11 |
| §2 | B + 2× `secondary-vga` (00:08, 00:09) | same as B | 11 |

**Ports (`info usb`):**

- With explicit `port=`, B and D place 2 keyboards on one controller and the
  mouse and the stick on one controller each, all at 12 Mb/s and with no hub.
- Without `port=`, the second keyboard landed behind an automatic hub at
  port 2.1.
- C put all four devices behind a single `usb-hub` on port 1.

**Kernel boot with the §2 line:**

- `pci: 12 function(s)`, including all three `class 0c03 prog_if 00`.
- `hda: stream=4 irq=11`, so HDA came up as normal.
- The shell ran `help` from `sendkey`.

**CI guard.** The `usb_topology` suite fails if this topology drifts from §2:

- the count of UHCI functions is not 3;
- a UHCI function lands on IRQ 0, 1 or 2;
- BAR 4 is not a 32-byte I/O block;
- a controller has any interrupt source enabled in `USBINTR`, which is what
  makes sharing HDA's line safe (§3);
- any frame list, QH, TD or TD data buffer in the schedule SeaBIOS left
  running lies in RAM the memory map calls usable.

It logs each controller's pin, line, registers and schedule walk to serial,
for example `usb_topology: uhci 6.0 pin 2 irq 11` and
`usb_topology: uhci 5.0 schedule: 19585 targets, 0 in usable RAM`. The
frame list for the controller with only the stick on it has fewer targets
(5121), because SeaBIOS keeps no interrupt pipe on it.
