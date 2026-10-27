# Driver: USB host controllers, core and HID keyboard

**Files:** `src/usb.c/h` (core + host-controller interface), `src/uhci.c/h`
(UHCI), `src/usb_kbd.c/h` (HID boot keyboard), `usb_poll()` hook in
`src/pit.c`, `usb_init()` in `src/kernel.c`. Tests:
`tests/kernel/test_usb_topology_k.c` (`usb_topology`),
`tests/kernel/test_usb_kbd_k.c` (`usb_kbd_report`),
`tests/kernel/test_usb_k.c` (`usb`), and `docker/scripts/qmp-keys.sh`.
**Status:** ✅ UHCI, enumeration and HID boot keyboards (SCRUM-198), built
on the design the SCRUM-245 spike settled (§1–§6). Mouse (SCRUM-246), mass
storage (SCRUM-247) and xHCI (SCRUM-248) are still open.
**Last updated:** 23 Oct 2026

---

## Table of Contents

1. [Purpose](#1-purpose)
2. [Topology in QEMU](#2-topology-in-qemu)
3. [Interrupt delivery: polled from IRQ0](#3-interrupt-delivery-polled-from-irq0)
4. [Root ports: several UHCI controllers, no hub](#4-root-ports-several-uhci-controllers-no-hub)
5. [QEMU input routing](#5-qemu-input-routing)
6. [Constraints on the stories that follow](#6-constraints-on-the-stories-that-follow)
7. [Layering: host-controller interface, core, class drivers](#7-layering-host-controller-interface-core-class-drivers)
8. [UHCI: takeover and schedule](#8-uhci-takeover-and-schedule)
9. [Enumeration](#9-enumeration)
10. [HID boot keyboard](#10-hid-boot-keyboard)
11. [What SCRUM-199 builds on](#11-what-scrum-199-builds-on)
12. [Tests and QMP key injection](#12-tests-and-qmp-key-injection)
13. [Appendix: measurements](#13-appendix-measurements)

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
(`exodoom-qemu` image, `pc`/i440fx machine, SeaBIOS). §13 has the raw data.

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
  Port 2 hub, Port 2.1 keyboard" (§13, run A). With no hub driver (§4), that
  second keyboard would be invisible.
- `ich9-usb-uhciN` works standalone on `pc`, with no EHCI companion. Several
  `piix3-usb-uhci` instances work just as well (run D). They differ only in
  device ID, and PIIX3's UHCI always signals on INTD.

## 3. Interrupt delivery: polled from IRQ0

**Decision:** the controllers run with `USBINTR = 0` and are polled from
`irq0_handler()` (`src/pit.c`), the same way `speaker_tick()` and `hda_tick()`
are serviced. The hook is a static-flag no-op until a controller is brought up.

**Why not wire the INTx line:** in every topology measured, at least one UHCI
function lands on IRQ 11 with `intel-hda` (§13). Today's command line already
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
enumerate. SCRUM-198's driver uses less than this: it reads registers only
in a health check every 16 ticks, and not at all on a controller with no
pipes (§8). Hot-plug (`PORTSC` connect-status-change) is noted here and acted on
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
kernel (§13):

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
`device` naming a console). SCRUM-198 confirmed the QMP route: `input-send-event` with `"device":"vga2"`
reaches the keyboard bound to `vga2` and no other (§12).

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
  - Until that driver existed, those running controllers were safe only
    because every structure they touched sat in memory the e820 map marks
    reserved, which `page_alloc` never hands out (§13). The `usb_topology`
    suite walked the firmware's schedule to check that. Since SCRUM-198 it
    checks instead that the takeover replaced it (§8).
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

## 7. Layering: host-controller interface, core, class drivers

Three layers, so that SCRUM-248 (xHCI) only has to write the bottom one
(§6):

| Layer | File | Knows about |
|---|---|---|
| Host controller driver | `src/uhci.c` | registers, TDs/QHs, root ports |
| USB core | `src/usb.c` | descriptors, addresses, the device table |
| Class drivers | `src/usb_kbd.c` | one device class each |

A controller driver fills in a `usb_hcd_ops_t` (`src/usb.h`) and calls
`usb_hcd_register()`:

| Op | Contract |
|---|---|
| `port_count` | root ports found by probing (§4) |
| `port_reset` | reset and enable one port; `USB_ENODEV` if nothing is connected, else the device's speed |
| `port_disable` | take a port's device off the bus (§9) |
| `control` | synchronous control transfer on endpoint 0, busy-waited |
| `interrupt_open` | start polling an interrupt-IN endpoint; each completed packet goes to a callback **from IRQ0** |
| `bulk` | synchronous bulk transfer; `USB_ENOTSUP` until SCRUM-247 |
| `poll` | the IRQ0 service (§3 budget) |
| `pipe_live` | whether an interrupt pipe is still delivering (not stalled, unplugged or halted) |
| `delay_ms` | busy-wait on the controller's own frame clock |

`delay_ms` exists because enumeration runs from `kernel_main` with
interrupts off (no blanket `sti` before the `TESTING` branch, see
`src/kernel.c`), so `kernel_sleep_ms()` would `hlt` forever. UHCI counts
`FRNUM`, which the controller advances every 1 ms frame by itself. That is
also the clock the USB spec's own timings are written against.

`usb_init()` (from `kernel_main`, after `pci_init()` and `ata_init()`, ahead
of the `TESTING` branch) calls `uhci_probe_all()`, enumerates every root
port of every registered controller (§9) and only then sets the flag that
lets `usb_poll()` run. Before that flag, the IRQ0 hook returns at once.

Class drivers are rows in `g_drivers[]` in `src/usb.c`. Each is offered the
device's whole configuration descriptor and returns 1 if it claimed the
device. `usb_config_find_interface()` does the descriptor walk for them.

## 8. UHCI: takeover and schedule

**Takeover**, per controller, before anything else (§6): write `0x8F00` to
`LEGSUP` (PCI config `0xC0`), `USBINTR = 0`, stop (`USBCMD = 0`, wait for
`HCH`), `GRESET` for 50 ms, `HCRESET` until it clears. The devices are then
back at address 0, unconfigured, whatever SeaBIOS did to them. The
`usb_topology` suite checks the result: `FRBASEADD` is the driver's frame
list, `RS` is set and `HCH` is clear.

**DMA memory**: three `alloc_page()` pages per controller, each checked to
be below 4 GB (UHCI pointers are 32-bit) and zeroed:

| Page | Holds |
|---|---|
| frame list | 1024 link pointers |
| sched | interrupt skeleton QH (+0), control QH (+16), then one 128-byte slot per interrupt pipe: QH, TD, packet buffer (max `UHCI_MAX_PIPES` = 8) |
| ctrl | control-transfer TDs (up to 62) at +0; setup packet at +2048; data from +2064 |

**Schedule** — fixed, never rebuilt:

```
frame list[0..1023] ──► int skeleton QH ──► pipe QH n ──► … ──► pipe QH 0 ──► control QH ──► T
                                              │                   │              │
                                              ▼                   ▼              ▼
                                           1 IN TD             1 IN TD     SETUP/DATA/STATUS chain
                                                                            (only during a transfer)
```

Every frame visits every pipe. The `bInterval` in the endpoint descriptor is
ignored: a HID device NAKs until its report changes (we send `SET_IDLE(0)`),
so a NAK is all a frame costs, and a 1 ms poll matches the 1 kHz PIT that
collects results anyway (§3). A new pipe's QH is fully built before the
skeleton's head pointer is switched to it, so a frame in progress sees
either the old chain or the whole new one.

**Control transfers** build SETUP (DATA0), data packets (from DATA1,
alternating) and STATUS (DATA1, opposite direction). The TDs are chained
depth-first and hung on the control QH, and the CPU walks the chain as the
controller retires it. Data-IN TDs carry `SPD`: a short packet parks the QH
on that TD, and the driver re-points the QH at the STATUS TD. Without `SPD`,
the next IN TD would run against a device that has already moved on to the
status stage. A transfer times out after 500 frames (plus an absolute spin
cap in case `FRNUM` itself stops). Afterwards the chain is unhooked and one
frame passes before its TDs are reused.

**Interrupt pipes**: one TD per pipe, with MaxLen set to the endpoint's
full `wMaxPacketSize`. A smaller MaxLen would make a device that sends a
full packet babble on every TD. `uhci_poll()` (IRQ0) skips a controller
with no pipes entirely. On the others, a normal tick reads only TD status
words in RAM, and no port I/O happens. No TD sets `IOC`, so `USBSTS` would
have nothing to report about completions. For each pipe whose TD has gone
inactive, it does one of three things:

- **Success:** pass the packet to the class callback, flip the data toggle,
  and re-arm. Re-arming is three dword writes: token, status, and QH element.
- **CRC/timeout or babble:** re-arm with the same toggle, because the packet
  never landed. After 8 failed TDs in a row (each already retried 3 times by
  the controller), the pipe is parked as dead.
- **STALL:** park the pipe as dead. Clearing a halt needs `CLEAR_FEATURE`,
  which is a control transfer and is outside the ISR budget.

Every 16th tick there is also a **health check**, and it is the only place
the hook reads registers:

- If `USBSTS` shows `HCH`, `HSE` or `HCPE`, the controller has stopped
  itself, so it is marked halted and every pipe on it is retired.
- If a pipe's root port shows no device (`CCS` clear), or a connect change
  (`CSC`, an unplug and replug), that pipe is retired.

Re-enumerating a replugged device would need control transfers, so it isn't
done here, and hot-plug isn't supported yet. `pipe_live` reports all of
these states.

During enumeration every busy-wait (control transfers, `delay_ms`, port
reset) also stops at once if the controller halts. Without that, a dead
controller would spin each step to the absolute cap with interrupts off.

**Takeover failures** stop the controller. Its pages are freed only once it
has provably halted. If it won't halt, they are leaked rather than handed to
someone else while it may still be reading them. The PCI I/O decode and
bus-mastering bits are enabled before the first register access, because
firmware without USB legacy support need not leave them on.

A completed TD that IRQ0 hasn't collected yet just waits: QEMU NAKs the
device's next report until the TD is re-armed, and `usb-kbd` queues up to
16 key changes meanwhile.

## 9. Enumeration

Per connected root port, with no hub layer (§4):

1. `port_reset`: `PR` for 50 frames, clear it, set `PE` (retrying while the
   port leaves reset), clear `CSC`/`PEC`, then 10 frames of reset recovery.
   Speed comes from `PORTSC.LSDA` and sets each TD's `LS` bit.
2. `GET_DESCRIPTOR(device, 8)` at address 0 with an 8-byte endpoint 0, to
   learn `bMaxPacketSize0`.
3. `SET_ADDRESS` with the next free address, starting at 1 in enumeration
   order, then 2 ms of `TSETADDR` recovery.
4. Full device descriptor, the 9-byte configuration descriptor, then the
   whole configuration (capped at `USB_CONFIG_MAX` = 256 bytes).
5. Offer the configuration to each class driver.

If any step before `SET_ADDRESS` fails, the port is disabled. Otherwise the
device would still be answering at address 0 when the next port's device is
reset and addressed there too. When the device table is full, the port is
never reset, so it stays disabled from `HCRESET`.

A device no driver claims stays addressed but unconfigured, and gets one
serial line. On the §2 topology the boot log reads:

```
uhci: 0:5.0 io 0x0000C040 ports 2 frame list 0x008E8000
uhci: 0:6.0 io 0x0000C060 ports 2 frame list 0x008EB000
uhci: 0:7.0 io 0x0000C080 ports 2 frame list 0x008EE000
usb: hc 0 port 0: addr 1 id 00000627:00000001 iface 00000003/00000001/00000001 -> usb_kbd
usb: hc 0 port 1: addr 2 id 00000627:00000001 iface 00000003/00000001/00000001 -> usb_kbd
usb: hc 1 port 0: addr 3 id 00000627:00000001 iface 00000003/00000001/00000002 (unclaimed)
usb: hc 2 port 0: addr 4 id 000046F4:00000001 iface 00000008/00000006/00000050 (unclaimed)
usb: 3 controller(s), 4 device(s), 2 keyboard(s)
```

The mouse (HID boot protocol 2) waits for SCRUM-246, and the stick (mass
storage, SCSI, Bulk-Only Transport) waits for SCRUM-247. The run targets
don't attach the mouse (§5), so `docker-run*` shows three devices.

## 10. HID boot keyboard

`usb_kbd_probe()` claims an interface that is class 3 / subclass 1 /
protocol 1 and has an interrupt-IN endpoint. It sends `SET_CONFIGURATION`,
`SET_PROTOCOL(boot)` and `SET_IDLE(0)`, which is optional, so a STALL is
ignored. It then opens the pipe at the endpoint's own packet size (up to
64 bytes, `USB_KBD_MAX_PACKET`) and decodes only the first 8, which are the
boot report.

The boot report is a modifier byte, a reserved byte, and up to six usage IDs
of the keys that are held right now. A report is a **state**, not an event,
so `decode()` diffs each one against the previous report:

1. If any key slot holds `0x01`–`0x03` (ErrorRollOver, POSTFail,
   ErrorUndefined), the report says "too many keys" rather than which keys,
   so it is dropped and the previous state kept. Treating it as "everything
   released" would emit false key-ups.
2. **Modifier bits that changed** become press or release events for
   `KEY_CTRL`, `KEY_SHIFT_LEFT`/`_RIGHT` and `KEY_ALT`. The per-keyboard
   `MOD_*` mask is updated *before* the key's own event, matching
   `update_modifier_state()` in `src/ps2.c`. Left and right Ctrl/Alt share a
   key but set distinct `MOD_*` bits. The GUI keys have no `ps2_key_t`, so
   they produce no event and no mask change.
3. **Releases** are usages that were in the old report and are not in the
   new one.
4. **Presses** are the reverse. Because releases come first, a key swapped
   for another in a single report reads as up, then down. Presence is checked
   by value rather than by array slot, because a held key can move between
   slots.

Usages map to the existing `ps2_key_t` (`usb_kbd_usage_to_key()`: letters,
digits, F1–F12, arrows, Enter/Esc/Tab/Space/Backspace, and the punctuation
the enum has). An unmapped usage queues nothing. The events are the same
`kbd_event_t` that the PS/2 path produces, so a consumer can treat either
source the same way.

**One ring per keyboard** (`kbd_ring_t`, `src/kbd_ring.h`, 256 events,
counts drops). Its single producer is the interrupt callback, running in
`irq0_handler()`, and its single consumer is `usb_kbd_pop()`. That meets the
ring's SPSC contract, and keeps USB input off the PS/2 ring entirely.

## 11. What SCRUM-199 builds on

- `usb_kbd_count()`, `usb_kbd_pop(idx, &ev)`, `usb_kbd_dropped(idx)`.
- `usb_kbd_live(idx)`: 0 once the keyboard's pipe has stalled, the device
  has been unplugged, or its controller has halted (§8).
- `usb_kbd_ident(idx, &id)`: controller index, root port and USB address.
  The root port is what stays stable across boots on a fixed topology
  (QEMU's `port=`). Addresses are handed out in enumeration order.
- **A ring is consumed only once a context is bound to it** (SCRUM-199,
  `src/kbd_source.c`, keyboard.md §7a): that context's `exo_kbd_poll` pops
  it via `usb_kbd_pop()`. A launcher binds one with `exo_kbd_bind` (#44, SCRUM-297); until it does,
  nothing consumes the ring, and an unbound ring fills to 255 events and
  then counts drops. A USB Ctrl+Tab is an ordinary key: the context-switch
  hotkey is decoded in the PS/2 path only. The source id is `usb:N`, N the
  enumeration index above (`KBD_SRC_USB(n)`).

## 12. Tests and QMP key injection

| Suite | File | Needs hardware |
|---|---|---|
| `usb_topology` | `test_usb_topology_k.c` | yes. §13's CI guard, plus a check that every controller runs the kernel's schedule (§8) |
| `usb_kbd_report` | `test_usb_kbd_k.c` | no. `decode()` driven on device-less slots (`usb_kbd_test_attach()`, `TESTING` only) |
| `usb` | `test_usb_k.c` | yes. Controllers, addresses, claimed keyboards, live pipes, pages below 4 GB, and the acceptance test |

**The acceptance test** ("two USB keyboards and PS/2 deliver independently")
needs real keystrokes during a headless test boot:

- `docker-test` and `docker-ci` add `-qmp tcp:127.0.0.1:4444,server=on,wait=off`.
- Just before QEMU starts, they launch `docker/scripts/qmp-keys.sh` in the
  background. The script is pure bash `/dev/tcp`, since the `exodoom-qemu`
  image has no socat or python.
- It retries the connection until QMP listens, then loops for as long as
  QEMU lives. Each round sends `input-send-event` for `a` (down, then up)
  with `"device":"vga2"` and then for `b` with `"device":"vga3"`, about
  every 200 ms.
- Each `usb-kbd` is bound to one of those consoles by `display=` (§5), so
  keyboard A types only `a` and keyboard B only `b`.

The test drains any stale events and feeds a PS/2 `z` (make and break)
through `ps2_process_scancode()`. That is the call `irq1_handler()` makes
for each byte, and it stands in for IRQ1, which isn't wired in a test
build. The test then opens an interrupts-on window of up to 5 s so
`usb_poll()` runs, and asserts four things:

- keyboard 0 saw `a` and not `b`;
- keyboard 1 saw `b` and not `a`;
- neither USB keyboard saw any other key, the `z` included;
- the PS/2 ring holds exactly the `z` press and release, and nothing else.
  `kbd_enqueue()` is the only way onto that ring, so a USB path that
  called it would show up here even with IRQ1 unwired.

A passing run logs:

```
usb_kbd: kbd0 a=1 b=0 | kbd1 a=0 b=1 | ps2 z=1 other=0 (3 ms)
```

The window usually closes at once, because the keys typed during earlier
suites are already waiting in `usb-kbd`'s queue. Separately, a non-test boot
was checked to still take PS/2 typing at the shell with the USB driver live
(HMP `sendkey` and a `screendump`).

## 13. Appendix: measurements

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
- a controller is not running the kernel's own schedule: `FRBASEADD` is
  not `src/uhci.c`'s frame list, `RS` is clear, or `HCH` is set. Before
  SCRUM-198 this check walked the schedule SeaBIOS left running and failed
  if any of it lay in usable RAM. The driver's reset (§8) retired that
  schedule.

It logs each controller's pin, line, registers and frame list to serial,
for example `usb_topology: uhci 6.0 pin 2 irq 11` and
`usb_topology: uhci 5.0 frbase 0x008EA000 driver 0x008EA000`. The spike's
firmware-schedule walk, which this suite ran before SCRUM-198, measured
`19585 targets, 0 in usable RAM` on the keyboards' controller and 5121 on
the stick's, which has no interrupt pipe.
