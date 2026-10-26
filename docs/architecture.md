# ExoDoom Architecture

**Last updated:** 18 Sep 2026 — sprint roadmap and epic breakdown reconciled
against the Jira board

Diagrams of the architecture, memory layout, syscall flow, interrupt path and more
live in [`docs/diagrams/`](diagrams/README.md).

---

## Table of Contents

1. [Project goal](#1-project-goal)
2. [What is an exokernel?](#2-what-is-an-exokernel)
3. [System layers](#3-system-layers)
4. [Boot sequence](#4-boot-sequence)
5. [Kernel subsystems](#5-kernel-subsystems)
   - [5.1 Memory](#51-memory)
   - [5.2 Interrupts (IDT / PIC / ISR)](#52-interrupts-idt--pic--isr)
   - [5.3 Timer (PIT)](#53-timer-pit)
   - [5.4 Serial](#54-serial)
   - [5.5 Framebuffer and console](#55-framebuffer-and-console)
   - [5.6 Keyboard](#56-keyboard)
   - [5.7 Real-time clock (CMOS RTC)](#57-real-time-clock-cmos-rtc)
6. [Syscall interface](#6-syscall-interface)
7. [LibOS and doomgeneric](#7-libos-and-doomgeneric)
8. [Testing infrastructure](#8-testing-infrastructure)
9. [Build and toolchain](#9-build-and-toolchain)
10. [Sprint roadmap](#10-sprint-roadmap)

---

## 1. Project goal

ExoDoom is a bare-metal x86_64 exokernel whose primary goal is to run
[Doom](https://github.com/ozkl/doomgeneric) directly on hardware, without a
conventional operating system underneath it. The project serves as both a
systems programming exercise and a demonstration that an exokernel can provide
just enough OS abstraction for a real-world application.

The end state is a bootable ISO that loads the Doom engine as a **LibOS**
(library operating system), maps the framebuffer, handles keyboard and mouse
input, and runs the game loop — all on a kernel with fewer than ~25 syscalls.

---

## 2. What is an exokernel?

A conventional OS kernel (Linux, Windows, etc.) abstracts hardware behind a
rich, opinionated interface: file systems, processes, virtual memory, sockets.
Application code never touches hardware directly.

An **exokernel** takes the opposite approach: it exposes hardware resources
(physical pages, the framebuffer, I/O ports) as directly as possible, and pushes
all policy decisions — how to manage memory, how to schedule, what a "file"
means — into user-space **library operating systems** that run per-application.

In ExoDoom's model:

- The **kernel** owns hardware and enforces protection (no LibOS can corrupt
  another's pages or access hardware it hasn't been granted).
- The **LibOS** is a thin user-space layer that implements just enough OS
  abstraction for Doom: a `malloc` heap, a `FILE*` shim, format-string printing,
  and the six `DG_*` platform functions doomgeneric requires.
- **Doom itself** is unchanged — it calls standard C library functions and the
  doomgeneric platform API, completely unaware it is running on bare metal.

**Protection model (secure binding).** The protection guarantee above is not a
property the kernel gets for free from paging — it is enforced by *resource
ownership tracking*, which is what distinguishes this from a monolithic kernel
that merely exposes a few syscalls. The kernel tags every physical page and the
framebuffer with an owner (`KERNEL` / `FREE` / a LibOS context id); resource
syscalls establish, enforce, and reclaim those bindings, rejecting any
cross-owner access with `-EPERM`. This is the exokernel's "secure binding"
mechanism: the kernel protects a resource without managing how the LibOS uses
it. The kernel also reserves the right to *revoke* a granted resource
(repossession) — the half of the bargain that makes generous grants safe, since
the kernel never has to refuse a request it could not later undo. The full rule
set lives in `docs/syscall_spec.md` §3.3, the revocation protocol in §3.6; the
work is tracked under epic SCRUM-151 (Resource Protection & Secure Binding).

Two pieces of this mechanism have landed. The PMM carries a per-page owner tag
(`page_owner_t` in `src/page_alloc.c`), `exo_page_alloc` stamps the calling
context as owner, and `exo_page_free` returns `-EPERM` for a page the caller
does not own (SCRUM-152). The framebuffer is bound the same way
(`src/fb_binding.c`, SCRUM-154): `exo_fb_acquire` binds it to one LibOS at a
time (`-EBUSY` to anyone else), and `fb_binding_check_map` is the gate that
makes mapping its physical pages require that binding. Framebuffer pages need
their own table because MMIO lies outside the RAM the page allocator manages,
so the per-page tags cannot speak for them.

Revocation is the third piece (`src/revoke.c`, SCRUM-156): a kernel-internal
`revoke(context, resource)` path that marks a resource in the ownership table
("I want this back"), lets the owner return it the ordinary way, and takes it
if the owner does not. The mark changes nothing else — a marked page keeps its
owner and stays mappable — because a request the LibOS cannot answer is not a
request. Forced reclamation is scoped to the context it names, so it can never
free a peer's page, and `revoke_all(context)` sweeps everything one context
holds. The policy on top is deliberately trivial for the single-app demo:
nothing asks for a resource back on the boot path, and `revoke_all()` is the
hook `exo_exit` reclamation (SCRUM-155) will call. The full protocol, and what multi-LibOS
scheduling still has to add to it, is `docs/syscall_spec.md` §3.6.

The fourth piece closes the hole the model was written for. `exo_page_map`
(SCRUM-35, `src/vmm.c` + `src/syscall_mem.c`) lets a LibOS build its own
address space a page at a time — and refuses any `paddr` it does not own or
hold the framebuffer binding for, `-EPERM` (SCRUM-153). It also confines the
`vaddr` to a window above 64 TiB, because until each LibOS has its own page
tables (SCRUM-48) a mapping call edits the kernel's, and owning a page must not
become a licence to install it over kernel text. The base is that high because
the kernel map is an *identity* map: a window below the top of physical memory
would overlap real kernel mappings. `docs/syscall_spec.md` §3.7.

```
┌──────────────────────────────────────────────────────┐
│                    Doom engine                       │  (unchanged doomgeneric source)
│   game logic · renderer · AI · physics · WAD I/O    │
├──────────────────────────────────────────────────────┤
│              doomgeneric platform shim               │  DG_Init, DG_DrawFrame, DG_GetKey …
├──────────────────────────────────────────────────────┤
│                  LibOS / libc shim                   │  malloc, printf, FILE*, math stubs
├──────────────────────────────────────────────────────┤
│            Exokernel  (ExoDoom kernel)               │  21 syscalls  ·  syscall
├────────────┬──────────┬──────────┬───────────────────┤
│  Physical  │  Timer   │  Input   │   Framebuffer     │  bare hardware
│  memory    │  (PIT)   │ PS/2 kbd │   (VESA/BGRX)     │
└────────────┴──────────┴──────────┴───────────────────┘
```

---

## 3. System layers

### Kernel (ring 0)

Written in freestanding C (gnu99) and x86_64 assembly. No libc. No external
dependencies beyond `libgcc` for compiler builtins. Responsible for:

- Physical memory management (page allocator, mmap parsing)
- Virtual memory (4-level page tables, `CR3`)
- Interrupt handling (64-bit IDT, PIC remapping, IRQ dispatch)
- Hardware drivers: PIT, PS/2 keyboard, PS/2 mouse, serial (COM1), framebuffer
- The syscall gate (`syscall`/`sysret`) and all 21 `exo_*` syscall implementations
- A minimal ramdisk filesystem for config and save files

### LibOS (ring 3, planned)

A per-application user-space library linked directly into the application
binary. For the Doom LibOS this means:

- A bump/slab heap backed by `exo_page_alloc` calls
- A `FILE*` abstraction backed by `exo_file_*` — a LibOS descriptor library
  over ExoFS, not a syscall (`docs/filesystem.md` §6d) — or a memory-mapped
  WAD shortcut
- Format-string printing (`printf`, `fprintf`, `vsnprintf`) routed through
  `exo_serial_write`
- Math stubs (`floor` as integer cast, one-time `sin`/`cos`/`tan` for lookup
  table init)
- The six doomgeneric platform functions (`DG_Init`, `DG_DrawFrame`,
  `DG_SleepMs`, `DG_GetTicksMs`, `DG_GetKey`, `DG_SetWindowTitle`)

### doomgeneric

The unmodified [doomgeneric](https://github.com/ozkl/doomgeneric) source — 82 C
files implementing the full Doom engine on top of a 6-function platform
interface. The only planned modifications are patching `I_GetEvent()` to source
mouse events from `exo_mouse_poll` rather than SDL.

---

## 4. Boot sequence

```
BIOS / UEFI
    │
    ▼
GRUB 2  (grub.cfg: gfxpayload=keep, multiboot2 /boot/exodoom)
    │  loads kernel ELF64 at 2M, loads freedoom2.wad as multiboot2 module
    │  sets up framebuffer via VESA, passes multiboot2 info struct* in %ebx
    │  enters kernel in 32-bit protected mode
    ▼
_start  (src/boot.s, .code32 — 32-bit trampoline)
    │  Multiboot 2 header: magic 0xE85250D6, framebuffer request tag (1024×768×32)
    │  saves MB2 info pointer from %ebx → %edi
    │  zeroes 24 KiB of page table memory (PML4 + PDPT + 4×PD)
    │  builds 4 GB identity map with 2 MB pages:
    │    PML4[0] → PDPT, PDPT[0..3] → PD0..PD3, each PD has 512 × 2 MB entries
    │  loads CR3 with PML4 address
    │  enables PAE (CR4 bit 5)
    │  sets EFER.LME (MSR 0xC0000080 bit 8) — enables long mode
    │  sets CR0.PG (bit 31) — activates paging → long mode active
    │  loads 64-bit GDT (null + code 0x08 + data 0x10)
    │  ljmp $0x08, $_start64
    ▼
_start64  (src/boot.s, .code64 — 64-bit entry)
    │  sets segment registers (DS/ES/FS/GS/SS) to 0x10 (data selector)
    │  sets RSP to stack_top (16 KiB stack in BSS)
    │  zero-extends EDI → RDI (MB2 info pointer, System V AMD64 ABI first arg)
    │  call kernel_main
    ▼
kernel_main  (src/kernel.c)
    │
    ├─ serial_init()          — COM1 at 38400 baud, FIFO enabled
    ├─ framebuffer tag        — locate MB2 tag (type 8); serial diagnostics only here
    ├─ mmap_init(mb)          — parse MB2 memory map tag (type 6)
    ├─ memory_init()          — set bump allocator base to align_up(&_bss_end, 4K)
    ├─ acpi_init(mb)          — RSDP→RSDT/XSDT→FADT, copied to statics; MUST precede vmm_init (SCRUM-250)
    ├─ page_alloc_init(mb)    — bitmap PMM + owner table, kernel/WAD reserved
    ├─ vmm_init(mb, fb)       — build the kernel page tables, load CR3 (SCRUM-15)
    ├─ syscall_init()         — EFER.SCE / STAR / LSTAR / FMASK
    ├─ syscall_mem_init()     — bind exo_page_alloc / exo_page_free
    ├─ syscall_fb_init(fb)    — publish FB geometry, bind exo_fb_acquire
    ├─ ata_init() + ata_register_blkdevs() — probe all four IDE positions (CD-ROM skipped), register each disk as a block device; the primary master takes the reserved system slot, device 0 (SCRUM-244)
    ├─ usb_init()             — take every UHCI from the BIOS, enumerate root ports, claim HID boot keyboards; after pci_init/ata_init, interrupts still off (SCRUM-198)
    ├─ syscall_disk_init()    — bind exo_disk_* (#24–#27, #31, #37) over the registry; after every driver that registers disks
    ├─ rtc_init() + syscall_rtc_init() — cache the FADT century register, bind exo_rtc_read (#35); after pit_init (SCRUM-249)
    │
    │  [if compiled with -DTESTING]
    ├─ run_tests()            — KUnit test runner, exits QEMU with pass/fail code
    │
    │  [normal boot]
    ├─ fb_init_bgrx8888() + fbcon_init()  — framebuffer console (halts if absent)
    ├─ idt_init()             — fill all 256 16-byte IDT entries with default_stub, lidt
    ├─ pic_remap()            — remap PIC1→0x20, PIC2→0x28 (avoids BIOS conflict)
    ├─ idt_set_gate(32, irq0_stub) — wire IRQ0 to PIT handler
    ├─ pit_init(1000)         — PIT channel 0 at 1000 Hz (1 ms tick)
    ├─ boot_sanity_check()    — gate: paging live + a real syscall round
    │                           trip (alloc/map/unmap/free); halts with a
    │                           FATAL message on serial + screen otherwise
    │                           (SCRUM-191)
    ├─ idt_set_gate(33, irq1_stub) — wire IRQ1 to keyboard handler
    ├─ kbd_init()             — PS/2 keyboard initialised, IRQ1 unmasked
    ├─ sti                    — enable interrupts
    │
    └─ shell LibOS launch (context_create → libos_build_image →
       vmm_switch_address_space → libos_enter_irq) — does not return;
       the shell is the rest of this boot (SCRUM-110/-178/-191)
```

Key details:

- The kernel is linked at **virtual address 2M** (`linker.ld`: `. = 2M`). The
  boot trampoline identity-maps the first 4 GB with 2 MB pages, and `vmm_init()`
  then replaces that with a narrower identity map of only the memory that exists
  (§5.1) — so virtual == physical throughout, framebuffer included (typically at
  ~0xFD000000 in the PCI MMIO aperture).
- **Anything the ring-3 tests need must be initialised above the `TESTING`
  branch**, which exits QEMU and never returns. That is why the PMM, the kernel
  page tables and the syscall bindings sit there, and the framebuffer console,
  IDT, PIC, PIT and keyboard below.
- GRUB enters the kernel in **32-bit protected mode** per the Multiboot 2 spec.
  The trampoline in `boot.s` transitions to 64-bit long mode before calling
  `kernel_main`.
- GRUB passes the MB2 info struct address in `%ebx`. The trampoline saves it in
  `%edi`, which is zero-extended to `%rdi` — the first argument register in the
  System V AMD64 ABI.
- The 64-bit half of the trampoline (`_start64`) also **enables SSE** before
  calling `kernel_main` (SCRUM-177): CR0.EM/TS cleared, CR0.MP/NE and
  CR4.OSFXSR/OSXMMEXCPT set, `MXCSR` loaded with `0x1F80`. It sits in the
  64-bit half rather than beside the CR4.PAE write because long mode is
  already active there, and AMD64 requires SSE2/FXSAVE of any CPU that can
  enter it — so no CPUID check is needed or present. See `docs/syscall_spec.md`
  §3.4a.
- The Multiboot 2 info struct uses a **tag-based format**. Tags are iterated
  with `mb2_find_tag()` to locate the framebuffer (type 8), memory map (type 6),
  and modules (type 3).
- The framebuffer is initialised **before** interrupts, so the boot banner and
  the `boot_sanity_check()` verdict are displayed on screen during early boot
  (SCRUM-191) — the boot flow no longer dumps the full memory map or a timer
  demo on a normal boot; it goes straight into the shell LibOS once the
  sanity check passes.
- The WAD file (`freedoom2.wad`) is declared as a GRUB module and is already
  mapped in physical memory by the time `kernel_main` runs. Its address and size
  are in a Multiboot 2 module tag (type 3).

---

## 5. Kernel subsystems

### 5.1 Memory

**Files:** `src/memory.c`, `src/memory.h`, `src/mmap.c`, `src/mmap.h`,
`src/page_alloc.c/h`, `src/vmm.c/h`, `src/multiboot2.h`

**Current state (Sprint 1):** Two-phase design, partially complete.

**Phase 1 — Multiboot 2 mmap parsing** ✅ Done (SCRUM-6): `mmap_init()` locates
the memory map tag (type 6) in the Multiboot 2 info struct and stores up to 32
`mmap_region_t` records (base, length, type). Usable RAM regions are type 1
(`MB2_MMAP_AVAILABLE`). On QEMU `-m 256M` this yields a large usable region
starting just above 1M.

**Phase 2 — Bump allocator** ✅ Done: `memory_init()` sets
`placement_address = align_up(&_bss_end, 4K)`. `kmalloc(size)` bumps the pointer
forward, always aligned to 4K. No `free`. Used during early boot only, before
the page allocator is ready.

**Phase 3 — Bitmap page allocator** 🔄 In Review (SCRUM-7): `alloc_page()` /
`free_page()` operating on 4K physical pages, with a bitmap stored in the
bump-allocated region. Double-free detection is required.

**Phase 4 — Kernel/WAD reservation** 🔄 In Progress (SCRUM-8): The allocator
must mark pages occupied by the kernel image (`_load_start`→`_bss_end`) and the
WAD module as unavailable, so they are never handed out.

**Phase 5 — Kernel page tables** ✅ Done (SCRUM-15): paging is on from the
`boot.s` trampoline (a static 4 GB identity map of 2 MB pages in `.bss`, built
only to reach long mode), but the map the kernel actually runs on is built by
`vmm_init()` in `src/vmm.c` from PMM pages — `PAGE_OWNER_KERNEL`, so no LibOS
can free one — and loaded into `CR3` before the `TESTING` branch in
`kernel_main`. It identity-maps low memory, the kernel image and bump pool,
every usable RAM region (WAD module included), the multiboot info and the
framebuffer aperture, and nothing else; page 0 is left unmapped as a NULL
guard. 2 MB leaves are used where alignment allows and split on demand when a
4 KiB mapping lands inside one, which is the primitive `exo_page_map`
(SCRUM-35/-153) is built on. Cost on QEMU `-m 256M`: 8 pages, plus one table
per 2 MB of LibOS address space a mapping syscall touches. A page fault
is reported and halts (SCRUM-17, `src/fault.c`). Still ahead: per-section
permissions and a read-only WAD (SCRUM-16), and per-LibOS address spaces
(SCRUM-48). Full detail in `docs/memory.md` §7.

**Memory map (QEMU, at boot, pre-paging):**

```
0x00000000 – 0x000FFFFF   reserved / low memory (BIOS, VGA)
0x00100000 – 0x001FFFFF   reserved (below kernel load address)
0x00200000 – ~0x00280000  kernel image (_load_start → _bss_end, ~512K typical)
~0x00280000 – ...          bump allocator free pool
...
[WAD module]               freedoom2.wad, placed by GRUB above kernel
[framebuffer]              physical address from mb->framebuffer_addr (VESA)
...
0x10000000                 top of 256M QEMU RAM
```

---

### 5.2 Interrupts (IDT / PIC / ISR)

**Files:** `src/idt.c`, `src/idt.h`, `src/isr.s`, `src/pic.c`, `src/pic.h`,
`src/io.h`

The x86_64 IDT has 256 entries, each **16 bytes** (vs. 8 bytes in 32-bit mode).
ExoDoom initialises all of them during `idt_init()`:

1. Every entry is filled with `default_stub` — a bare `iretq` in `src/isr.s`.
   The code segment selector is hardcoded to `0x08` (the kernel's own GDT code
   segment).
2. `idt_load()` is called via the System V AMD64 ABI — the IDT pointer address
   is passed in `%rdi`, and `lidt (%rdi)` loads the IDT register.

The 8259A PIC ships with IRQ vectors 0–15 mapped to CPU exception vectors 0–15,
which would conflict. `pic_remap()` reinitialises both PICs via ICW1–ICW4 to map
IRQ0–7 → vectors 0x20–0x27 and IRQ8–15 → 0x28–0x2F. All IRQs except IRQ0
(timer) and IRQ1 (keyboard) are then masked.

IRQ stubs in `src/isr.s` manually save caller-saved registers (`rax`, `rcx`,
`rdx`, `rsi`, `rdi`, `r8`–`r11`) via `PUSH_REGS`/`POP_REGS` macros, call the
C handler, restore registers, and execute `iretq`. The 64-bit `pusha`/`popa`
instructions do not exist in long mode.

> ⚠️ **Open issue (SCRUM-135):** CPU exception vectors that push error codes
> (8, 10–14, 17, 21, 29, 30) still use `default_stub` which does a bare
> `iretq`. A dedicated `error_stub` that pops the error code before `iretq`
> must be installed before paging refinement work begins.

**Handler status:**

| Vector | Exception                | Handler status                                       |
| ------ | ------------------------ | ---------------------------------------------------- |
| 13     | General Protection Fault | ✅ `gpf_stub` → `gp_fault_handler` (SCRUM-56): decoded frame, RIP, CS:RSP; halts. Reuses `page_fault_handler`'s TESTING resume hook; no CR2, no useful error-code bits for the port-I/O case it exists to prove |
| 14     | Page Fault               | ✅ `pf_stub` → `page_fault_handler`: CR2, decoded error code, RIP, live mapping; halts |
| 32     | IRQ0 / Timer             | ✅ `irq0_stub` → `irq0_handler`                      |
| 33     | IRQ1 / Keyboard          | ✅ `irq1_stub` → `irq1_handler`                      |
| 44     | IRQ12 / Mouse            | ⬜ Sprint 2 (SCRUM-19)                                |

---

### 5.3 Timer (PIT)

**Files:** `src/pit.c`, `src/pit.h`, `src/sleep.c`, `src/sleep.h`

**Status:** ✅ Done (SCRUM-9, SCRUM-10)

The Intel 8253/8254 PIT (Programmable Interval Timer) has three channels.
ExoDoom uses channel 0, wired to IRQ0. `pit_init(hz)` programs the divisor as
`1193180 / hz` (the PIT's base clock is 1.193180 MHz) using mode 3 (square
wave). The project runs the PIT at **1000 Hz** — one IRQ0 every 1 ms.

`irq0_handler()` increments a `volatile uint32_t ticks` counter and sends EOI to
the PIC. `kernel_get_ticks_ms()` returns `ticks * 1000 / frequency`.
`kernel_sleep_ms(ms)` busy-waits by polling `kernel_get_ticks_ms()` and
executing `hlt` to yield until the next interrupt.

This timer feeds directly into `DG_GetTicksMs` and `DG_SleepMs` once the LibOS
is wired up. The game loop runs at 35 tics/second and calls `DG_GetTicksMs` ~35
times per second.

**PC speaker (SCRUM-98):** PIT channel 2 generates PC speaker tones
(`src/speaker.c/h`, `docs/drivers/speaker.md`) without interfering with
channel 0. `speaker_tone(freq, dur_ms)` returns immediately; `irq0_handler()`
calls `speaker_tick()` each tick to end a timed tone, so the
`exo_sound_tone` syscall (SCRUM-100, `src/syscall_sound.c`) is non-blocking.

---

### 5.4 Serial

**Files:** `src/serial.c`, `src/serial.h`

**Status:** ✅ Done

COM1 (I/O base `0x3F8`) is the kernel's primary output channel throughout
development. Configured at 38400 baud, 8N1, with the FIFO enabled. All kernel
diagnostic output and the entire KUnit test suite output goes here. QEMU maps
COM1 to `stdio` via `-serial mon:stdio`.

`serial_putc()` busy-waits on the Line Status Register's transmit-empty bit.
`serial_flush()` waits for the transmit holding register to clear. The kernel
has no interrupt-driven serial receive; COM1 is output-only during normal
operation.

`exo_serial_write(buf, len)` will be the syscall that routes LibOS
`printf`/`fprintf` output through this driver with an address-space validation
check.

---

### 5.5 Framebuffer and console

**Files:** `src/fb.c`, `src/fb.h`, `src/fb_console.c`, `src/fb_console.h`

**Status:** ✅ Implemented and active — wired into the boot path. The boot
banner and `boot_sanity_check()` verdict (SCRUM-191) are displayed on the
framebuffer console before the shell LibOS takes it over.

GRUB sets up a VESA linear framebuffer and passes its physical address, pitch,
width, height, and bpp in a Multiboot 2 framebuffer tag (type 8). The empirically confirmed
pixel format on QEMU is **BGRX8888** — each pixel is 4 bytes in memory order
`[B][G][R][X]`, equivalent to the 32-bit value `0x00RRGGBB` on a little-endian
machine.

`fb_init_bgrx8888()` validates bpp == 32 and populates a `framebuffer_t`.
`fb_clear()` and `fb_fill_rect()` write pixels directly to the linear address.
There is no double-buffering yet.

`fb_console_t` implements an 8×16 character cell text console (an 8×8 bitmap
font doubled vertically) on top of the framebuffer. It supports
foreground/background colour, cursor rendering, newline/carriage return/tab, and
scrolling by `memmove`-ing the framebuffer up by 16 pixels and clearing the
bottom row.

**Secure binding (SCRUM-154, superseded by SCRUM-112 below):**
`exo_fb_acquire()` originally returned the framebuffer's real physical address
and geometry to the LibOS *and* bound the framebuffer to it — one owner at a
time, `-EBUSY` to anyone else, `-ENODEV` on a machine with no framebuffer. The
binding table, `src/fb_binding.c`, still exists and its API is unchanged, but
`exo_fb_acquire` (`src/syscall_fb.c`) no longer calls into it — see the
multiplexing note just below for what replaced it. `DG_DrawFrame` will blit
the 640×400 RGBA8888 `DG_ScreenBuffer` into the LibOS's mapped region (with
format conversion, since Doom produces RGBA and the hardware is BGRX),
unaffected by which mechanism handed that region out.

**✅ Sprint 12: Framebuffer multiplexing (SCRUM-112).** Every context that
calls `exo_fb_acquire()` now gets its own private, RAM-backed virtual
framebuffer, sized to the real framebuffer's published geometry — no
exclusivity, no `-EBUSY`; `src/fb_shadow.c` is the per-context directory,
`alloc_pages_contig_owned()` (`src/page_alloc.c`) the allocator behind it. The
LibOS-side flow (`exo_page_map` the returned `phys_addr`, per SCRUM-36's
`libos_fb_map()`) is unchanged — it never notices its buffer is private RAM
rather than real MMIO. `src/fb_compositor.c` is what makes any of it visible:
on a throttled PIT tick (`src/pit.c`, ~60 Hz) it copies whichever context is
`context_current()`'s virtual framebuffer onto the real hardware one.
"Foreground" is simply "the context currently running" — no separate state,
matching the cooperative model where exactly one context executes at a time —
which is what makes context switches (a `wadview` launch, `exo_exit` back to
the shell, a future `exo_yield`-driven scheduler) show the right app's own
display state on the next tick with no extra signaling. This closes the
consequence the old binding note above used to flag ("while a LibOS holds the
framebuffer, the kernel's own `fb_console` must stop drawing to it"): the
kernel's own boot-time console draws directly to the real hardware buffer
before any LibOS launches and is never composited over, since nothing has
acquired a virtual framebuffer yet at that point.

**The boot hand-off (SCRUM-192).** The boot shell is the one exception to
"a new context's buffer starts black". `kernel_main` creates the shell's
virtual framebuffer itself with `fb_shadow_seed_from_screen()`, a copy of the
boot console as it stands, ending in "Launching shell LibOS...". The
compositor shows the shell's buffer from its first syscall, and the shell
spends its first stretch mapping that buffer page by page and then clearing
it. With a zeroed buffer the screen sat black for all of that, which
under QEMU emulation was seconds. Measured with `-icount shift=8`, that
stretch was ~950 ms of guest time and ~850 ms of it was `fb_clear()`'s
per-pixel loop. That loop is now a `rep stosl` per row (`src/fb.c`), and the
compositor copy and shadow zeroing are `rep movsq`/`rep stosq`
(`src/fb_copy.h`) instead of `src/string.c`'s byte loops. Launch to banner
drops from ~1350 ms to ~960 ms of guest time with no black frame. Seeding is
only safe because nothing but the kernel has drawn yet; every other launch
keeps a zeroed buffer, or it would see another context's pixels.

**✅ SCRUM-111:** the Ctrl+Tab hotkey this note originally left undone.
"Foreground == running" turned out not to need a scheduler to interrupt —
`context_switch_request()` (`src/context.c`) already lets *anything*
request a switch, not just the running context itself, so §5.6's IRQ1
handler calls it directly on the chord. See §5.6 and
`docs/syscall_spec.md` §3.5's SCRUM-111 note for the mechanism and its one
remaining caveat (the low-level CR3/register swap itself still lands on
the outgoing context's next syscall, not instantly) — SCRUM-179 fixed the
correctness half of that same window (`context_current()` used to flip
before the swap actually happened, misattributing the outgoing context's
own syscalls in the meantime).

---

### 5.6 Keyboard

**Files:** `src/ps2.c`, `src/ps2.h`, `src/isr.s` (IRQ1 stub), `src/idt.c` (gate)

**Status:** ✅ IRQ1 handler and scan code processing complete (SCRUM-13,
SCRUM-14) / ✅ Event ring buffer complete (SCRUM-18)

The PS/2 keyboard controller maps to IRQ1 (IDT vector 33). When a key is pressed
or released, the controller raises IRQ1 and places a scan code (Set 1) in port
`0x60`. `irq1_handler` drains every buffered byte from `0x60` and sends EOI.

SCRUM-13 implements the IRQ1 handler to read raw scancodes. SCRUM-14 implements
the Set 1 → key enum translation table, tracking make/break codes and modifier
state (shift, ctrl, alt). SCRUM-18 adds the 256-event ring buffer
(`src/kbd_ring.c`) so rapid input isn't dropped: the IRQ path only decodes and
enqueues — no serial I/O — and `kbd_service()` drains the queue from ordinary
kernel context. SCRUM-39 exposes the queue to the LibOS as the
`exo_kbd_poll(event_out)` syscall.

PS/2 mouse (IRQ12, port `0x60`/`0x64`, 3-byte packets) follows in Sprint 2
(SCRUM-19) and feeds `exo_mouse_poll`.

**USB keyboards** (SCRUM-198, `src/uhci.c`, `src/usb.c`, `src/usb_kbd.c`)
are the second, independent keyboard that dual Doom needs: the 8042's
second channel is the mouse. They are added alongside PS/2 and do not
replace it. The UHCI controllers are polled from `irq0_handler()` with
`USBINTR = 0` rather than taking an IRQ. Each HID boot report is diffed into
`kbd_event_t`s on a ring of that keyboard's own, identified by controller,
root port and address. No syscall reads them yet; routing is SCRUM-199.
Details: `docs/drivers/usb.md` §7–§11.

> ✅ **SCRUM-111 (Ctrl+Tab switch hotkey):** `ps2_process_scancode()` checks
> for Ctrl+Tab (either Ctrl — `MOD_LCTRL | MOD_RCTRL`) right after
> `update_modifier_state()` runs, before the event would otherwise be
> queued. On a match it calls `context_next_ready()`/
> `context_switch_request()` (`src/context.c`) — the same round-robin pair
> `exo_yield` uses (`src/syscall_yield.c`), including the same
> `PAGE_OWNER_FREE` no-op when nothing else is `READY` — and swallows the
> keystroke instead of enqueueing it, so it never reaches an app as
> ordinary input. Calling straight into `context.c` from the IRQ1 path
> mirrors `irq0_handler()`'s existing direct call to
> `fb_compositor_tick()` (`src/pit.c`, SCRUM-112). See §5.5's SCRUM-111
> note and `docs/syscall_spec.md` §3.5 for why no new "foreground" state
> was needed, what the one remaining latency caveat is, and how SCRUM-179
> closed the correctness gap this hotkey originally opened
> (`context_current()` used to flip before the real CR3/register swap,
> misattributing the outgoing context's own syscalls in the deferred
> window).

### 5.7 Real-time clock (CMOS RTC)

`src/rtc.c/h` reads the CMOS clock through ports `0x70`/`0x71` and
`src/syscall_rtc.c` exposes it as `exo_rtc_read` (#35): calendar date and time
in UTC plus a derived Unix time. Polled only — IRQ8 stays masked. A read waits
out the update-in-progress flag and repeats until two reads agree, honours
Status B (BCD vs binary, 12- vs 24-hour), and takes the century from the ACPI
FADT (`docs/drivers/acpi.md`, SCRUM-250), assuming 20xx without one. The PIT's
`kernel_get_ticks_ms()` is unchanged and is *not* anchored to wall time. Details:
`docs/drivers/rtc.md`.

---

## 6. Syscall interface

**Full specification:** [`docs/syscall-spec.md`](syscall-spec.md)

Syscalls will use the `syscall`/`sysret` instruction pair (x86_64 fast syscall
mechanism). The syscall number will be in `RAX` and arguments in `RDI`, `RSI`,
`RDX`, `R10`, `R8`, `R9` (following the System V AMD64 convention with `R10`
replacing `RCX` since `syscall` clobbers it). Return value in `RAX`; negative
values are error codes.

The 21 syscalls grouped by category:

| Category    | Syscalls                                                            | Status     |
| ----------- | ------------------------------------------------------------------- | ---------- |
| Memory      | `exo_page_alloc`, `exo_page_free`, `exo_page_map`, `exo_page_unmap` | ✅ done     |
| Framebuffer | `exo_fb_acquire`                                                    | ✅ done     |
| Timer       | `exo_get_ticks`                                                     | Sprint 3   |
| Input       | `exo_kbd_poll`, `exo_mouse_poll`                                    | Sprint 3   |
| Debug       | `exo_serial_write`                                                  | ✅ done     |
| File I/O    | none — #9–16 reserved, never bound; `exo_file_*` is a LibOS library over ExoFS (SCRUM-44) | n/a |
| Sound       | `exo_sound_tone`, `exo_sound_stop` ✅ SCRUM-100                     | Sprint 11  |
| Scheduling  | `exo_yield`                                                         | ✅ done     |
| Lifecycle   | `exo_exit`                                                          | Sprint 3   |

### 6.1 Syscall entry (implemented, SCRUM-32)

The gate is the x86_64 fast `syscall` instruction, not an `int 0x80` trap gate
— an earlier i386-era plan that SCRUM-150 retired. `syscall_init()`
(`src/syscall.c`) programs four MSRs at boot: `EFER.SCE` to enable the
instruction, `STAR` with the segment selectors, `LSTAR` with the entry point,
and `FMASK` with the RFLAGS bits to clear on entry.

`sysret` computes its selectors arithmetically from `STAR[63:48]` rather than
loading them, which fixes the GDT layout in `src/boot.s`: kernel code/data at
`0x08`/`0x10`, then user code32 (`0x18`, a required placeholder), user data
(`0x20`) and user code64 (`0x28`). Reordering those breaks every syscall
return, so the layout is asserted in `tests/kernel/test_syscall_k.c`.

Unlike an interrupt, `syscall` does not switch stacks — the entry stub
(`src/syscall_entry.s`) does it, onto a dedicated 16 KiB kernel stack, then
preserves **every** register except `RAX`/`RCX`/`R11` before calling
`exo_syscall_dispatch`. That is stronger than the SysV callee-saved set; see
`docs/syscall_spec.md` §3.4 for why, and for the entry path in full.

Handlers register into the dispatch table with `exo_syscall_register()` from an
`*_init()` that `kernel_main` calls ahead of the `TESTING` branch. Bound today:
`exo_page_alloc` / `exo_page_free` (#0, #1, SCRUM-34), `exo_page_map` /
`exo_page_unmap` (#2, #3, SCRUM-35) in `src/syscall_mem.c`, and
`exo_fb_acquire` (#4, SCRUM-154) in `src/syscall_fb.c`, and `exo_serial_write`
(#8, SCRUM-50) in `src/syscall_serial.c`. Every other number returns
`-EXO_ENOSYS`.

---

## 7. LibOS and doomgeneric

**Status:** Not yet started — planned Sprint 3 onwards.

### WAD loading

`freedoom2.wad` is passed as a GRUB Multiboot 2 module (tag type 3). Its physical
address and size are available in the module tag. The recommended approach is a
**memory-mapped WAD reader**: `fopen("freedoom2.wad")` in the LibOS returns a
fake `FILE*` backed by a pointer into the module's mapped memory, and
`fread`/`fseek` operate as offset arithmetic over that region. This avoids
implementing any real file I/O for the game's largest data source.

> ✅ **SCRUM-164:** the WAD is now a genuine `IWAD`, fetched at build time
> rather than committed. `docker/scripts/build.sh` downloads the official
> Freedoom v0.13.0 release zip from `github.com/freedoom/freedoom`, checks it
> against that release's GPG-signed `CHECKSUM` file, extracts `freedoom2.wad`,
> and caches the result in `build/freedoom2.wad` (host-bind-mounted, so a
> valid download survives across builds); either hash mismatch fails the
> build loudly rather than shipping a bad WAD. Swapping WADs going forward is
> a URL + hash edit in `build.sh`, not a multi-megabyte commit.
>
> Current pin: Freedoom v0.13.0's `freedoom2.wad`, 28,787,748 bytes, sha1
> `975f781e6d801c0a23e3caa33f70493efe68a880`, magic `IWAD` — verified by
> extracting it from a zip whose sha256 matched the signed release checksum.
> An earlier pin (an archive.org-hosted file) also had `IWAD` magic but
> matched no official Freedoom release by size or hash across v0.11–v0.14.0
> alpha when checked against this same repo, so it was dropped in favor of
> this one. The file shipped before that was a `PWAD` (a patch layered on a
> base game) rather than a complete `IWAD`, which would have surfaced as a
> startup failure deep in
> `W_Init`/`Z_Malloc`/`R_Init` (SCRUM-81) with a stack trace pointing at the
> allocator or renderer instead of the real four-byte cause — worth
> remembering if a future WAD swap reintroduces the same class of bug. `src/grub.cfg`'s
> `module2 /boot/freedoom2.wad freedoom2.wad` line and `build.sh`'s ISO-staging
> step now both exist; before this ticket neither did, so no module tag ever
> reached the kernel despite `page_alloc.c` already being able to reserve one.

### libc shim scope

doomgeneric requires 82 C source files' worth of standard library. `string.h`
(SCRUM-11, plus `strcasecmp`/`strncasecmp`), `ctype.h`, `printf`→serial
(SCRUM-20) and `stdlib.h`'s allocation, numeric and sorting families
(SCRUM-30: `malloc`/`free`/`realloc`, `atoi`, `abs`, `rand`/`srand`, `qsort`)
are in. The most significant gaps remaining are `strdup`, `strncmp`,
`strrchr`, `strstr`, `strerror`, `vsnprintf`, `sscanf`, `exit`/`abort`, and the
full `FILE*` interface. See `docs/syscall_spec.md` §2 for the complete audit.

> ✅ **SCRUM-64:** all 79 files under `src/doom/` now compile to `.o` with zero
> errors (`make docker-build-doom`, now a CI gate that exits non-zero on any
> failure). It was 2 of 79 before. Almost all of it was missing *headers*
> rather than anything wrong with the vendored source: 77 files stopped at
> their first `#include` — 66 of them on `<strings.h>`, which `doomtype.h`
> includes on every non-Windows build — long before the compiler could reach a
> real error. `src/strings.h`, `src/inttypes.h`, `src/errno.h`(`+.c`),
> `src/math.h`, `src/unistd.h`, `src/assert.h`, `src/fcntl.h`,
> `src/sys/types.h` and `src/sys/stat.h` are new, the doom compile pass now
> gets `-I src`, and `src/stdio.h` grew the `FILE*` declarations.
>
> **Compiling is not linking.** Everything in that new `stdio.h` block, plus
> `mkdir`, `strdup` and `atof`, is *declared and undefined* on purpose — the
> objects compile and would fail at link with undefined references, which is
> the honest state rather than stubs that let Doom link and then misbehave
> inside `W_Init`. `docs/libc_audit.md` (SCRUM-72) is the per-function,
> per-call-site list of what is still owed.
>
> ✅ **SCRUM-177: SSE is enabled.** Doom requires it and cannot be compiled
> without it — the x86_64 SysV ABI returns `float` in `xmm0`, so
> `m_config.c`'s `M_GetFloatVariable()` has no SSE-free encoding (`-msoft-float`
> and `-mfpmath=387` both still hit the ABI). The doom compile pass drops
> `-mno-sse`; the kernel build keeps it and is unaffected (no `src/*.c` uses a
> float). `src/boot.s`'s `_start64` now clears CR0.EM/CR0.TS, sets
> CR0.MP/CR0.NE and CR4.OSFXSR/CR4.OSXMMEXCPT, and loads `MXCSR` with the
> architectural default `0x1F80`, all before `kernel_main` runs. Only 4 objects
> hold float arithmetic (18 instructions), but allowing SSE also lets GCC
> inline struct copies with `movaps`/`movdqa`/`pxor` engine-wide (~143 more);
> both classes needed the same bits, and `tests/kernel/test_sse_k.c` covers
> both, in ring 0 and from a ring-3 LibOS.
>
> **FPU/XMM state is deliberately not saved across the syscall or interrupt
> paths**, and that is a recorded decision rather than an omission: the kernel
> is compiled `-mno-sse -mno-sse2 -mno-mmx` and its hand-written assembly
> touches only general-purpose registers, so it is *incapable* of modifying a
> LibOS's XMM/MXCSR/x87 state — an invariant the ring-3 tests check against a
> real syscall and a real IRQ0 taken at CPL 3, rather than one this paragraph
> merely asserts. It holds only while exactly one ring-3 context exists;
> **a real scheduler must add `fxsave`/`fxrstor` to `context_switch.s`.** Full
> reasoning and the list of what would invalidate it: `docs/syscall_spec.md`
> §3.4a.

> ✅ **SCRUM-51:** `malloc`/`free`/`realloc` and `printf` now go through the
> real `syscall` instruction, not a kernel function call, when compiled for
> the ring-3 LibOS link target SCRUM-173 introduced
> (`tests/kernel/libc_shim_probe/`) — `malloc` reaches `libos_heap_alloc`
> (SCRUM-38) → `libos_page_alloc` (SCRUM-37) → the real `exo_page_alloc`/
> `exo_page_map` stubs, and `printf` buffers a full line through the real
> `exo_serial_write` stub instead of one call per byte. The kernel build and
> every `tests/kernel/*.c` suite are unaffected — `src/stdlib.c`/`src/stdio.c`
> gate on `#ifdef EXO_KERNEL` and keep calling `kmalloc`/`serial_putc`
> there, exactly as before. `exo_get_ticks` (already bound, SCRUM-172) is
> proven reachable from the same real ring-3 call. See §9 (build-script notes)
> and `docs/memory.md` §8 for the mechanism.

### Linking the engine: the libc gate and the `libos_doom` target (SCRUM-65/66)

`nm` over the compiled engine lists 52 external symbols; 32 had no definition
before SCRUM-65. They all resolve now, and **`make docker-link-doom`
(`docker/scripts/link-doom.sh`, run by CI) is the gate that keeps it that
way**: it compiles the shim as a ring-3 LibOS target, sets it beside the doom
objects, and fails naming any libc symbol still undefined. The only survivors
are the `DG_*` platform callbacks, each allowlisted in that script **by ticket
number** — adding to the list without one is how it would rot into a set of
exemptions. (`make docker-build-doom` is the separate compile-only gate; it
needs all 79 `src/doom/` files to compile and exits non-zero if that
regresses.)

Since SCRUM-66 those 79 objects are also **linked into `build/exodoom`** as
a ring-3 LibOS: `build.sh` step `[2f/7]` builds the `libos_doom` target
through `build_ring3_link_target()` (§9), `src/syscall_launch.c` embeds the
resulting blobs and launches them on `exo_launch(EXO_LAUNCH_APP_DOOM)` (#21,
the single argument-based launch syscall SCRUM-184 collapsed four per-app ones
into), and the shell's `doom` command calls it. Doom then completes its whole
startup — `W_Init` over the mounted WAD, `R_Init`, `P_Init`, `S_Init`,
`HU_Init`, `ST_Init`, `I_InitGraphics` — and runs its tick loop. Page and
stack sizing for the launch are in `docs/libos-authoring.md` §4.7–§4.8.

Things the audit turned up that are worth knowing before touching this area:

- **`printf`'s missing precision was the highest-severity item in the whole
  libc audit**, and not for cosmetic reasons: `%.3d` fell to the `default:`
  branch, which echoed the text *and did not consume the vararg*, so every
  later conversion in the call read a shifted argument. `hu_stuff.c` builds HUD
  font lump names with `"STCFN%.3d"`, so Doom died in `I_Error` before the HUD
  ever drew. Precision, length modifiers, `%X`/`%o`/`%f`, `%n` and `*` width
  are all implemented now. `vprintf` was added for the fatal path (below), and
  `printf` is implemented in terms of it — one formatting engine and one sink
  per build.
- **`fopen` reads through a pluggable backend** (`exo_stdio_set_backend()`,
  `src/stdio.h`; SCRUM-42/75). With none installed it resolves a name against
  blobs registered via `exo_file_register_blob()` — the original shim, still
  what the kernel-side tests and every program without a filesystem get, with
  matching on the **basename**, case-insensitively. A program that has mounted
  ExoFS calls `exo_file_stdio_install()` (`src/libos_fs/exo_file_stdio.c`) and
  `fopen`/`fread`/`fseek`/`ftell`/`fscanf` then run over `exo_file_*`; a
  relative or `./` path is resolved against the volume root. The backend is a
  function table so `src/stdio.c` does not pull ExoFS into every target.
  **Writing works only through a backend that has a write op** (SCRUM-42):
  `fopen` accepts `r`/`w`/`a` and their `+` forms, `fwrite`/`fprintf`/`fflush`
  write through `exo_file_write`, and `mkdir` calls `exofs_mkdir`. With no
  backend, or one without a write op, `fopen` for writing fails `EROFS`: a
  write that silently went nowhere would surface much later as a config that
  never persists or a savegame that reloads as garbage. `remove`/`rename`
  still fail `EROFS` until SCRUM-106. Each stream buffers one 512-byte block
  of writes, flushed on full, `fflush`, `fclose`, `fseek` and `fread`; nothing
  is durable, and the file's size is not on disk, until `fclose`
  (`docs/filesystem.md` §6c, §6d). `mkdir`, `strdup`,
  `atof` and the `FILE*` block in `src/stdio.h` are real;
  `docs/libc_audit.md` remains the per-function, per-call-site list.
- **`src/fpconv.c` exists because `-mno-sse` forbids `double` in kernel
  sources.** All of `%f` and `atof`'s real logic is integer work on an IEEE-754
  bit pattern, so it compiles and is unit-tested from ring 0; what sits behind
  an **`__SSE2__` gate** in `stdio.c`/`stdlib.c` is two adapters that move 8
  bytes. Measured against glibc: bit-exact for |decimal exponent| ≤ 15, worst
  case 2 ULP. **The gate is `__SSE2__`, not `!EXO_KERNEL`** — a distinction
  that has already broken a build: `build.sh`'s `probe_cflags` is the kernel
  `CFLAGS` with only `-mcmodel` swapped, so a ring-3 target inherits
  `-mno-sse` unless it asks otherwise, and "not the kernel" does not imply
  "has SSE". Exactly one target asks: `libos_doom` passes `-msse -msse2`
  through `extra_cflags`, and then these paths light up on their own.
- **What SCRUM-64 fixed was overwhelmingly *missing headers*, not bad vendored
  source:** 77 files stopped at their first `#include`, 66 of them on
  `<strings.h>` alone (`doomtype.h` includes it on every non-Windows build).
  The doom pass also gets `-I src` so the shim's headers are reachable.
- **Fixed-point trigonometry** (`src/fixed_math.c/h`, SCRUM-41) is the whole
  of what the board calls "math.h"; see `docs/syscall_spec.md` §2.4.

### doomgeneric platform functions

| Function            | Backed by                                                |
| ------------------- | -------------------------------------------------------- |
| `DG_Init`           | mounts the kernel-mapped WAD ✅ SCRUM-73; framebuffer half still owed |
| `DG_DrawFrame`      | BGRX blit from `DG_ScreenBuffer` into mapped framebuffer |
| `DG_SleepMs`        | Busy-wait loop on `exo_get_ticks`                        |
| `DG_GetTicksMs`     | `exo_get_ticks`                                          |
| `DG_GetKey`         | `exo_kbd_poll` dequeue                                   |
| `DG_SetWindowTitle` | No-op (or `exo_serial_write` for debug)                  |

> ✅ **SCRUM-74:** `DG_GetTicksMs` and `DG_SleepMs` are implemented, in
> `src/doomgeneric_exo.c` — ExoDoom's doomgeneric platform file, the
> equivalent of upstream's `doomgeneric_sdl.c`. It lives in `src/` rather than
> `src/doom/` so the vendored tree (SCRUM-63) stays a clean drop-in on a
> re-vendor, and it includes `doom/doomgeneric.h` for the prototypes rather
> than restating them (which is why `build.sh`'s kernel C compile now passes
> `-I src`). The other four callbacks are deliberately left **undefined
> rather than stubbed** — nothing links `src/doom/` yet, so an undefined
> reference is the honest placeholder.
>
> It carries the same `#ifdef EXO_KERNEL` split as `src/libos_page_alloc.c`:
> a real ring-3 LibOS build calls the `exo_get_ticks()` stub, while the
> kernel build reaches `exo_syscall_dispatch()` directly, because a `syscall`
> executed from ring 0 would `sysretq` a CPL-0 caller down to CPL 3.
> `tests/kernel/test_doomgeneric_timer_k.c` drives all five checks from ring 0.
>
> ⚠️ **`DG_SleepMs` spins on a counter only IRQ0 advances, so it returns only
> with interrupts enabled.** That holds on a normal boot but **not** under
> `-DTESTING`, where `kernel_main` deliberately performs no blanket `sti`
> (several suites drive ring-3 faults with `RFLAGS.IF` hardcoded clear). A
> `DG_SleepMs(n>0)` with interrupts off never returns, and would surface as a
> CI timeout with no failing assertion — so every test that waits enables
> interrupts for just its own wait and clears them after, as
> `test_syscall_pit_k.c` already does. There is no internal bailout: a
> deadline needs a second time source, and if one existed `DG_SleepMs` would
> be using it.
>
> `DG_SleepMs` uses `pause`, not `hlt` — `hlt` is privileged and would `#GP`
> in ring 3, where this is actually meant to run. The elapsed-time
> subtraction is done in `uint32_t` so it stays correct across the counter's
> ~49.7-day wrap, matching `kernel_sleep_ms` and Doom's own `I_GetTime`.

### `DG_Init` mounts the program volume and checks the IWAD (SCRUM-73, SCRUM-75)

Doom reads its IWAD like any program reads a file. `freedoom2.wad` is on the
**program volume** — the ExoFS volume `install` copies packages and the IWAD
to (`docs/filesystem.md` "Install"). The `doom.exo` manifest asks for
`progvol` (`EXO_PKG_WANT_PROGRAM_RO`, `tools/exopkg`), so `exo_launch_pkg`
assigns that extent to Doom **read-only** (`docs/syscall_spec.md` §3.5a);
`DG_Init` learns it from `exo_disk_extent` (#37), binds it with
`exo_disk_acquire`, `exofs_mount`s it, installs the FILE* backend, then
`dg_check_iwad()` opens the file and checks the 12-byte header and reports on
serial. `D_FindIWAD` → `fopen` and `W_AddFile` (`src/doom/w_file_stdc.c`,
the stock upstream reader, unmodified) then read lumps with
`fseek`/`fread`; nothing is preloaded or mapped.

Until SCRUM-75 the kernel read the whole 28 MB IWAD into a contiguous buffer
(`src/doom_wad_disk.c`), `libos_map_wad()` mapped it at `LIBOS_WAD_VADDR`,
`g_doom_params` carried its address, and `DG_Init` registered it as a blob.
All of that is gone from Doom. `doom_wad_disk_get()`/`libos_map_wad()` remain
for the WAD viewer (`EXO_PKG_WANT_WAD`) and the kernel tests.

Two consequences worth knowing:

- **While Doom runs, the shell cannot bind the program volume.** Doom holds
  its read-only binding for its lifetime (the mounted volume's FAT is resident,
  and `exofs_unmount` before release is the rule). Readers share, but a writer
  overlapping a reader is `-EXO_EBUSY`, and the shell binds read-write, so
  `programs`/`run`/`install` report the volume busy until Doom exits. `exo_exit` releases the disk binding and any assigned volume (`src/syscall_exit.c`), so the volume is free the moment Doom quits, not at the next launch's reap.
- **Sound and music no longer walk a flat WAD.** `doom_sound_pcm.c` and
  `src/libos_doom/doom_sound_music.c` get lumps from `doom_wad_lump()`
  (`src/doom_wad.h`): the installed source if there is one, else the mounted
  WAD. Ring-3 Doom installs `engine_lump()` (`src/libos_doom/libos_doom.c`),
  which answers from the engine's own `W_CacheLumpNum(..., PU_STATIC)`, so
  an effect or GENMIDI is cached once and the pointer stays valid after the
  call. Kernel tests still mount a real buffer and use the fallback.

- **`DG_Init` reports rather than halts.** It records a `DOOM_WAD_*` code in
  `dg_init_result()` and returns. A missing or malformed IWAD is exactly what
  Doom's own `d_iwad.c` → `W_AddFile` path exists to diagnose, with engine
  context this layer lacks; halting first would pre-empt that for no gain. It
  also means no `#ifdef` splits the tested path from the shipped one —
  `tests/kernel/test_dg_init_k.c` drives the real function from ring 0.
- **`g_doom_params` is gone** (SCRUM-75): Doom no longer takes a launch params
  struct, so `doom.exo` has `params_len` 0 and `src/doomgeneric_exo.c` need not
  lead its `.data`. The entry-point rule (`.text.entry`) is unchanged.
- **Three fixed addresses inside the LibOS window.**
  `LIBOS_LAUNCH_CODE_VADDR`, `_DATA_VADDR` and `_STACK_VADDR`
  (`src/libos_launch.h`). Code and data+bss each span up to
  `LIBOS_LAUNCH_MAX_{CODE,DATA}_PAGES` = **192** pages (Doom uses ~97 and ~99;
  the limits were 4/4 for the first probe, 8/16 for the libc shim, and grew
  again for SCRUM-66). The stack is `LIBOS_LAUNCH_STACK_PAGES` = **16** pages
  (it was one page; Doom recurses through the BSP tree).
- **It is a fixed-size loader, not a general one.** `code`/`data` are copied
  verbatim into position — no ELF, no relocation — and oversized input is
  rejected as `VMM_EINVAL` before anything is allocated. `.bss` is zeroed on
  every launch, which later sections depend on (the Networking note below).
- **`LIBOS_LAUNCH_STACK_TOP` is `LAYOUT_END - 8`, not `LAYOUT_END`.** The
  SysV ABI guarantees `RSP ≡ 8 (mod 16)` on entry to a called function (the
  `call` pushed a return address). A LibOS entered by `iretq` pushes none, so
  it must be given that skew by hand or every frame is misaligned by 8 and the
  first aligned SSE access (`movdqa -0x40(%rbp)`) raises `#GP`. Doom was the
  first ring-3 target compiled *with* SSE, so it was the first to catch it.
- **The `libos_main()` entry convention (SCRUM-50).** Code runs at
  `LIBOS_LAUNCH_CODE_VADDR` and may call any bound syscall through the
  ordinary `exo_syscall.h` stubs. Hand-assembled probes
  (`tests/kernel/libos_main_probe.s`) must additionally be position
  independent — no internal jump or call, no RIP-relative reference, and a
  reference to their own data uses the fixed immediate
  `LIBOS_LAUNCH_DATA_VADDR`. *Compiled* targets get none of those
  restrictions, because the link target (§9) hands the compiler the real
  addresses. `test_libos_main_k.c` proves the convention by calling the real
  `exo_serial_write` (#8) and checking the genuine result returns through
  `libos_return()`, not a fixed marker.
- **Entry-point placement (SCRUM-175).** `libos_build_image()`'s `entry_vaddr`
  is `.text` offset 0. Mark the real entry function
  `__attribute__((section(".text.entry")))`; `*(.text.entry)` is placed first
  in `tests/kernel/ring3_link_target.ld.in`'s `.text` block. "List the entry
  file first" alone is not enough: at `-O0` GCC does not inline
  `exo_syscall.h`'s `static inline` syscall stubs away and emits them as real
  functions ahead of the entry point whenever they precede it in source order.
- **Argument hand-off (SCRUM-175).** A LibOS app declares its own params
  struct as literally the *first global* in its `.data`, with a non-zero
  initialiser so it lands in `.data` and not `.bss` (GCC stores no bytes for a
  zero initialiser, so there would be nothing to patch). The kernel-side
  launcher calls `libos_launch_patch_params()` to overwrite it in place via
  `img->data_paddrs[0]` — a physical page the kernel already has
  identity-mapped — right after `libos_build_image()` succeeds. This is the
  pattern every app (shell, WAD viewer, clock/calculator, Doom, whatever
  comes next) follows; `g_wad_params` in
  `src/libos_wad_viewer/libos_wad_viewer.c` with
  `sys_launch_wad_viewer()` in `src/syscall_launch.c` is the worked example,
  and it replaced an earlier WAD-viewer-specific side channel (a second
  hand-mapped page at its own fixed VA). Both this rule and the entry-point
  rule depend on the entry-and-params TU being **linked first** in
  `build_ring3_link_target()`'s source list: GCC emits a TU's globals into
  `.data` in source order and `ld`'s `*(.data)` collects input sections in
  link order. (`g_doom_params` in `src/doomgeneric_exo.c` used to follow the same
  rule; SCRUM-75 removed it — see "`DG_Init` mounts the program volume".)

### Launching from a package on disk (SCRUM-230)

`exo_launch(app_id)` (#21) can only start a program whose blob was linked into
the kernel image. `exo_launch_pkg(pkg, len)` (#36) is the other path: the
launcher is **ring 3** — today the shell's `run <name>` — which mounts the
ExoFS program volume (`LIBOS_WAD_DISK_BASE_LBA`), reads an `src/exo_pkg.h`
package into a `libos_heap_alloc()` buffer, unmounts and releases the disk,
and hands the bytes to the kernel. The kernel never sees a filesystem; it
validates the package, builds the image with the same `libos_build_image()`
and enters it through the same `context_prime_irq`/`context_switch_request`
tail as #21 (both syscalls share those helpers in `src/syscall_launch.c`).
Details and error codes: `docs/syscall_spec.md` §3.2 #36.

- **Where packages come from.** `docker/scripts/build.sh` step [2g/7] runs
  `package_ring3_target` for every ring-3 target (snake, clock, wadview, doom),
  reading the `build/<name>_{code,data}.bin` blobs and `<NAME>_BSS_LEN` that
  `build_ring3_link_target()` already leaves behind, through the host tool
  `tools/exopkg/` (which calls `exo_pkg_pack()`, not a second copy of the
  format). Output is `build/packages/*.exo`; `exopkg --check` re-parses each
  one so a bad manifest fails the build. `tools/mkexofsimg` then writes every
  package onto the program volume next to the IWAD (extra `<host-file> <name>`
  argument pairs), so the disk image is written *after* the ring-3 targets, not
  at step [1b/7]. Page counts are derived from the blobs; `--params-len`,
  `--sound-voices`, `--min-pages`, `--recommended-pages` and `--stack-pages`
  override them. A new target (Tetris, a calculator) is one more
  `package_ring3_target` line. SCRUM-237 gives a program a volume of
  its own on request: the manifest's `volume_slot`/`volume_sectors`
  (`src/exo_pkg.h`, layout in `src/exo_volume.h`) become a kernel-side extent
  assignment at launch, which the program reads with `exo_disk_extent` (#37)
  and binds with `exo_disk_acquire(0, base, count)` (#26; slots are always on
  device 0, the system disk, SCRUM-244). `--volume-slot N
  --volume-sectors M` on `tools/exopkg` sets them; `docker/scripts/build.sh`
  step [2f2/7] builds the `libos_voldemo` demo, packaged as `voldemoa.exo`
  and `voldemob.exo` (slots 0 and 1), and grows the disk image to cover the
  slots. `docs/syscall_spec.md` §3.5a has the rules; SCRUM-233 (install) and SCRUM-234 (first boot, below) are the
  two halves.
- **First boot (SCRUM-234).** The shell probes this volume at startup and offers
  to format it when blank; `programs` lists the packages on it. See
  `docs/filesystem.md` "First boot".
- **Install (SCRUM-233).** `install <name>` copies a file from the install
  disk (block device 1, an ExoFS volume of every package plus the IWAD that
  `build.sh` writes each build) onto this volume, after which `programs` lists
  it and `run` launches it. `install doom` means `/doom.exo`; the IWAD is a
  separate `install freedoom2.wad`. The copy mounts the install disk and the
  system volume together and moves a chunk at a time (SCRUM-265). See `docs/filesystem.md`
  "Install".
- **Snake and the clock are no longer in the kernel image.** Their
  `launch_apps[]` rows and `EXO_LAUNCH_APP_SNAKE`/`_CLOCK` are gone (WAD viewer
  is app id 0, Doom 1); `build.sh` builds their blobs but does not link them
  (`ring3_embed=0`), and the shell's `snake`/`clock` commands are `run
  snake.exo`/`run clock.exo`. The kernel half is asserted by the
  `launch_pkg` suite; the boot half is a manual `make docker-run-kernel` check.
- **Resource dashboard (SCRUM-236).** `src/libos_top/libos_top.c`,
  shipped as `top.exo` (`ring3_embed=0`, like the clock) and started by the
  shell's `top` command. Redraws ~4 Hz from #22/#23/#31/#38/#39 and #5; no syscall
  was added. It is a full-screen app you Ctrl+Tab to: the kernel runs one context
  at a time and the compositor shows only `context_current()`, so showing it
  beside a running program needs the preemptive scheduling of SCRUM-127/176.
  Contexts are listed by id and program name: the launcher passes the name to #36 (the shell uses the file name without `.exo`), the kernel names the shell and the #21 apps itself, and #23 reports it. The name is display-only and caller-asserted. The page redraws in place (one clear at start, every line padded to the row width) because clearing each frame let the compositor copy a black frame.
- **Admission (SCRUM-232).** The manifest's `min_pages`/`recommended_pages`/
  `sound_voices`/`want_flags` are read at launch. The kernel *reports* what is
  free (`exo_resstat`, #38: free pages, free context slots, voices, whether the
  framebuffer is held); the *launcher* decides, using `exo_admit()` in
  `src/exo_admit.h` (header-only, pure, tested with hand-built structs). Fewer
  free pages than `min_pages`, or no free context slot, is a refusal; pages
  between min and recommended, or fewer voices than wanted, is a degraded grant;
  a held framebuffer is *not* a refusal (displacing the holder is the designed
  launch, SCRUM-196) and is only reported. The shell's `run` prints the verdict,
  e.g. `run: refused: needs 271 pages, 40 free`, instead of the launch failing
  inside `libos_build_image()`. #36 keeps a backstop (`free_pages < min_pages`
  is `-EXO_ENOMEM` before anything is allocated) for a launcher that skips the
  check; it is a floor, not the policy. `min_pages` is not a guess:
  `exo_pkg_footprint_pages()` (`src/exo_pkg.h`) is the formula `exopkg` uses
  (code + data/bss + the fixed 16-page stack + 4 page-table pages, + 33 more for
  `EXO_PKG_WANT_WAD` since the WAD's tables are the program's own), and
  `test_launch_pkg_k.c` measures a real launch against it.
- **The grant read-out (SCRUM-235).** Admission says what was *asked* against
  what is *free*; the read-out says what the program *holds*, at the moment it
  starts. `run` launches with `EXO_LAUNCH_HOLD` (#36), so the context exists but
  has not run; reads it with `exo_grantstat` (#39: pages, framebuffer holder,
  disk extent, sound voices); writes it next to the manifest through
  `exo_grant_format()` (`src/exo_grant.h`, header-only and pure like
  `exo_admit.h`); then enters it with `exo_launch_start` (#40). The split is
  needed because #36 otherwise switches away immediately and the shell never
  runs again until the program yields. The read-out goes to COM1 only: the
  program draws over the console as soon as it runs, so an on-screen copy
  would vanish unread or need the shell to delay the launch. Nothing new is
  tracked: every field is read from the table that already enforces it. The framebuffer line states what
  *will* happen (the binding moves in #40), and "sound" reports voices
  *available*, since voices are owned at play time. Same data, continuously, is
  SCRUM-236.
- **Doom and the WAD viewer launch this way too.** Doom asks for
  `EXO_PKG_WANT_PROGRAM_RO` (SCRUM-75) and reads its own IWAD; the WAD viewer
  still uses `EXO_PKG_WANT_WAD` (with
  `params_len` = 16) makes #36 map the IWAD and patch `libos_wad_params_t`
  exactly as #21's `LAUNCH_NEEDS_WAD` rows do (shared `launch_attach_wad`), so
  the shell's `doom` and `wadview` are `run doom.exo` / `run wadview.exo` and go
  through the same admission check. The #21 rows, `EXO_LAUNCH_APP_*` and the
  embedded blobs are still in the kernel image: the `launch_multi` and fuzz
  suites drive them, and retiring them is follow-up work.
- **Limits worth knowing.** The header's `stack_pages` is advisory (the builder
  always maps `LIBOS_LAUNCH_STACK_PAGES`). `recommended_pages` is `min + 16`
  unless `--recommended-pages` overrides it; it is not a measurement of what a
  program's heap grows to.

### Multi-application (Sprint 12)

The eventual goal is **cooperative multitasking** between the Doom LibOS and a
minimal shell LibOS. `exo_yield` will trigger a context switch (save
registers + `CR3` swap), and a keyboard hotkey (Ctrl+Tab) will swap input
focus and the active framebuffer.

> ✅ **SCRUM-107:** the context table itself exists —
> `src/context.h`/`src/context.c`. Each entry (`context_t`) holds a
> `page_owner_t` id, a `context_regs_t` save area (the six callee-saved GPRs
> plus `RSP`/`RIP`/`RFLAGS` `src/libos_enter.s` needs to resume a launched
> context) and a `context_state_t` (`READY`/`RUNNING`/`BLOCKED`).
> `context_create()` binds a caller-built address space (typically from
> `vmm_create_address_space()`) to a freshly allocated id via
> `vmm_bind_address_space()` — the page-dir tracking stays in `vmm.c`'s own
> registry rather than being duplicated here, so there is one source of
> truth for "which PML4 does this context run on". Table capacity is
> `CONTEXT_MAX == VMM_MAX_ADDRESS_SPACES - 1` (15), since a context with no
> bound address space isn't meaningful in this design and one of vmm's
> `VMM_MAX_ADDRESS_SPACES` slots is permanently claimed at boot by
> `src/kernel.c`'s `PAGE_OWNER_LIBOS` placeholder bind.
> `tests/kernel/test_context_k.c` proves the acceptance criterion directly:
> 2+ real contexts, each on its own address space, tracked simultaneously.
>
> What this ticket does **not** do: an actual context switch — that is
> SCRUM-108, below.
>
> ✅ **SCRUM-108:** the switch itself. `context_prime()` seeds a never-run
> context's `context_regs_t` with an initial `iretq` frame
> (`entry_vaddr`/`stack_top_vaddr`/`LIBOS_LAUNCH_RFLAGS`); `context_switch_
> request(to_id)` validates `to_id` is a live `READY` context, flips the
> outgoing/incoming states, and arms a pending-switch flag read by
> `src/syscall_entry.s`. That file's epilogue, right after
> `exo_syscall_dispatch()` returns, checks the flag and — only when a switch
> was requested — jumps to `context_switch_tail` (`src/context_switch.s`)
> instead of its ordinary pop+`sysretq` restore: it captures the outgoing
> context's callee-saved GPRs (live, restored by the C call ABI) and
> `RCX`/`RFLAGS`/`RSP` (from the kernel stack slots `syscall_entry.s` itself
> pushed — **not** live registers, since `RCX`/`R11` are caller-saved in the
> C ABI and get clobbered by the C call chain in between; see
> `context_switch.s`'s own comment), swaps `CR3`, and `iretq`s into the
> incoming context's saved frame — the same shape `libos_enter()` builds for
> a fresh launch, whether the target was primed or is resuming from a
> previous switch-out. `tests/kernel/test_context_switch_k.c` proves the
> acceptance criterion directly: two real LibOS instances (`context_switch_
> probe_a.s`/`_b.s`) ping-pong through a test-local `EXO_SYS_YIELD` borrow,
> and callee-saved register values seeded before the first switch read back
> correctly after two hops and a resume.
>
> **Deliberately not done here:** the full `swapgs` + per-CPU
> (`IA32_KERNEL_GS_BASE`) rework `docs/syscall_spec.md` §3.4 and
> `src/libos_launch.h`'s `libos_enter()` comment both describe for
> `syscall_entry.s`'s `saved_user_rsp` and `libos_enter.s`'s
> `libos_saved_rsp`. This ticket's switch instead stages state through those
> same single globals at each switch boundary — correct because
> `IA32_FMASK` clears `IF` for the whole syscall/switch window and this
> kernel targets exactly one CPU, so no second entry can interleave. The
> full rework is SCRUM-176, needed once SCRUM-127 (preemptive, IRQ-driven
> switching) wants to switch context from inside an interrupt handler with
> `IF` set — the scenario this ticket's minimal fix does not cover. Nothing
> here is wired into `kernel_main`'s normal boot tail either, for the same
> reason SCRUM-47/-49/-50 landed unwired: there is still only one real LibOS
> to run on a normal boot, and `exo_yield` itself (binding a real syscall
> number to this mechanism) is SCRUM-109's job.
>
> ✅ **SCRUM-109:** `exo_yield` (#19) is bound for real. `src/syscall_yield.c`'s
> `sys_yield()` handler calls a new `context_next_ready()` (`src/context.c/h`)
> — a round-robin scan of the context table, starting just past the calling
> context's own slot and wrapping once — to pick a target, then hands it to
> `context_switch_request()` exactly as the SCRUM-108 test-local `SYS_SWITCH`
> handler did. This is deliberately the *minimum* policy the acceptance
> criterion needs, not a real scheduler: no priority, no fairness beyond
> plain round-robin, and `BLOCKED` contexts are simply skipped rather than
> woken. A full scheduler (preemption, priority, wake-on-event) is still
> SCRUM-147's job. If nothing else is `READY` — including the case where the
> caller itself has no row in the table, the boot-time default LibOS
> (`context_switch_request()`'s existing `CONTEXT_ENOENT` refusal) — `exo_yield`
> is a no-op: it returns `0` and the caller carries on, since the documented
> ABI (`docs/syscall_spec.md` §3.2 #19) has no error return.
> `tests/kernel/test_syscall_yield_k.c` proves the acceptance criterion
> directly through the real syscall and policy (not a test-local stand-in):
> two real LibOS instances (`tests/kernel/yield_probe_a.s`/`_b.s`, adapted
> from the SCRUM-108 probes to call `exo_yield()` with no argument) round-trip
> through two real `exo_yield()` calls with register state intact, plus a
> standalone no-op case.
>
> ✅ **SCRUM-110:** the first real, second LibOS a normal boot actually
> launches — not just a test harness. `src/shell/shell_main.c` is a minimal
> terminal: it maps the framebuffer via `libos_fb_map()` (`src/libos_fb.c`),
> draws a text console with the same `src/fb_console.c` the kernel's own boot
> banner uses (framebuffer-address-agnostic, so it compiles and runs
> unmodified as compiled ring-3 code), and polls `exo_kbd_poll()` in a loop,
> decoding `ps2_key_t` to ASCII and dispatching `help`/`clear`/`about` on
> Enter — anything else echoes `unknown command: <line>` (the command set and
> its dispatch have since moved to a table; see SCRUM-263 below). `src/ps2.h`/`.c`
> gained `KEY_0`–`KEY_9` and a handful of punctuation keys
> (`KEY_MINUS`/`_EQUALS`/`_COMMA`/`_PERIOD`/`_SLASH`/`_SEMICOLON`) alongside
> this so the shell has more than a letters-only command vocabulary.
> `docker/scripts/build.sh`'s `build_ring3_link_target` mechanism (SCRUM-51/
> -173) now runs **unconditionally**, not just under `TESTING=1` — the shell
> blob has to exist in every build, since `kernel_main` launches it on every
> normal boot, replacing the kernel-mode keyboard idle loop that used to sit
> at the end of `kernel_main` — via `libos_build_image(PAGE_OWNER_LIBOS, ...)`
> + `libos_enter_irq()` (the `_irq` entry point, not plain `libos_enter()`,
> since the shell needs `exo_kbd_poll`/`exo_get_ticks` to see live IRQ-driven
> state). Built as `PAGE_OWNER_LIBOS` itself — not a fresh context id —
> because `syscall_current_context()` (`src/syscall.c`) is still hardcoded to
> that one id in v1 (no real context switch has ever run on a normal boot),
> so the shell's own `exo_page_map`/`exo_fb_acquire` calls only resolve into
> the address space actually loaded in `CR3` if that is the address space
> bound to `PAGE_OWNER_LIBOS`; `libos_build_image()` rebinds it in place,
> replacing the placeholder binding to `vmm_kernel_pml4()` set up earlier in
> `kernel_main` — exactly the "whole of the change needed" this section's own
> SCRUM-48 note above anticipated. `tests/kernel/test_shell_libos_k.c` builds
> the real embedded blob through `libos_build_image()` and checks it fits the
> LibOS-window budget, but deliberately does not launch it: `shell_main()`
> never calls `libos_return()` — it is a genuine, never-returning interactive
> loop, launched once for the rest of a normal boot, not a probe — so the
> rest of the interactive behavior (typing, backspace editing, command
> dispatch) is verified manually via `make docker-run-kernel` rather than
> through a scripted return path. **Not done here:** any handoff back to
> Doom or real cooperative scheduling — Doom is still not linked into the
> kernel at all, so `exo_yield()`'s call in the shell's idle loop is a no-op
> exactly as documented (`docs/syscall_spec.md` §3.2 #19). **✅ SCRUM-112:**
> framebuffer multiplexing between two live LibOS instances is done — see
> §5.5's own note above; the WAD viewer this section already describes
> launching (`wadview`, SCRUM-178) is what proved it: the shell and the
> viewer no longer fight over the one real hardware buffer, each keeps its
> own display state, and switching between them shows the right one.
>
> **✅ SCRUM-111:** the Ctrl+Tab switch hotkey itself. `src/ps2.c` detects
> the chord (either Ctrl) in `ps2_process_scancode()` and calls the same
> `context_next_ready()`/`context_switch_request()` pair `exo_yield` uses,
> directly from the IRQ1 path — no new "foreground" state, since
> `context_switch_request()` already updates `context_current()`
> synchronously and §3.5's compositor note already reads exactly that.
> The chord is swallowed, not queued, so it never reaches an app as
> ordinary input. The low-level register/CR3 switch is still deferred to
> the outgoing context's next syscall (sub-frame in practice, since every
> LibOS here polls every loop iteration) — see `docs/syscall_spec.md`
> §3.5's SCRUM-111 note for the full latency caveat and why closing that
> gap for good is still SCRUM-176/-127's job, not this ticket's.
>
> Two bugs surfaced only once this hotkey made both halves of the round-
> robin reachable for the first time, and both are fixed as part of this
> ticket rather than left as follow-ups, since neither is optional for the
> acceptance criterion to actually hold:
>
> 1. `exo_exit` (`src/syscall_exit.c`) left the exiting context's row
>    `CONTEXT_STATE_READY` after handing off — correct for `exo_yield`
>    (the outgoing side really is resumable) but wrong for an exit (its
>    pages and framebuffer are already gone). Nothing before this ticket
>    was ever positioned to round-robin back into that stale row; Ctrl+Tab
>    at the shell right after quitting `wadview` did, and executed garbage
>    from freed pages. Now downgraded to `CONTEXT_STATE_BLOCKED`, which
>    `context_next_ready()` already skips.
> 2. `shell_main()`'s idle loop (`src/shell/shell_main.c`) called
>    `exo_yield()` unconditionally every iteration, on the assumption
>    (recorded in that file's own SCRUM-178-era comment) that the viewer
>    would eventually "yield in turn" and hand control back. It never does
>    — `run_automap_viewer()` only gives up control by fully exiting — so
>    that call was a harmless no-op right up until Ctrl+Tab could bring the
>    shell to the foreground while the viewer was still alive and `READY`.
>    At that point it became actively wrong: the shell's very next loop
>    pass immediately yielded straight back to the viewer via the same
>    round-robin, before a keypress could ever land at the shell — Ctrl+Tab
>    would arm and even complete the low-level switch, but the shell never
>    stayed foreground long enough to be usable. Removed outright: every
>    real hand-off away from the shell is already explicit (`wadview`, its
>    own `exo_exit`, or Ctrl+Tab), so nothing needs the shell to volunteer
>    control on its own.
>
> ⚠️ **A real GCC pitfall worth knowing before writing the next ring-3 link
> target:** `libos_build_image()` always treats byte 0 of the linked code
> blob as the entry point — no ELF symbol lookup, no `e_entry` — so whichever
> function GCC's default `-ftoplevel-reorder` (on by default from `-O1` up)
> happens to place first in the object's `.text` becomes the real entry,
> regardless of source order. `libos_c_probe.c` and `libc_shim_probe.c` never
> hit this because each keeps its entry function as the *only* function in
> its own object; the SCRUM-110 shell target has several helpers alongside
> `shell_main()` and hit it for real during development — GCC placed a small
> helper first, so the launched context executed the helper's bytes as its
> entry and immediately page-faulted writing into the (non-writable) code
> region. `docker/scripts/build.sh`'s `probe_cflags` now passes
> `-fno-toplevel-reorder` for every `build_ring3_link_target` target, fixing
> this for good rather than leaving it as a convention for the next multi-
> function target to rediscover.
>
> ✅ **SCRUM-263:** the shell's commands come from one table,
> `shell_cmds[]` in `src/shell/shell_main.c`. Each row holds the name, group
> (FS/DISK/APPS/SYSTEM), one-line summary, full usage text, handler, and
> whether the command takes arguments. Dispatch (case-insensitive on the
> command name only), `help`, `help all|<group>|<command>|<prefix>[*]`,
> `<cmd> ?` and every usage error all read that table, so adding a command
> is one row. Rows are listed in table order, so keep the table sorted. A
> group name wins over a command of the same name (`help disk` lists the
> group; `disk ?` shows the command). An argument of exactly `?` never
> reaches a handler, and trailing text on a no-argument command prints its
> usage instead of being ignored. The matching and row formatting live in
> `src/shell/shell_help.c`, which has no console or syscall dependency:
> `build.sh` links it into the shell and also compiles it into the TESTING
> kernel for `tests/kernel/test_shell_help_k.c`. `shell_key_to_ascii()`
> now maps Shift+`/`, `8`, `-` to `?`, `*`, `_`, the only shifted symbols
> the command syntax uses.

---

## 8. Testing infrastructure

**Files:** `src/kunit.h`, `tests/kernel/kunit.c`, `tests/kernel/test_runner.c`,
`tests/kernel/test_smoke.c`, `tests/kernel/test_string_k.c`,
`tests/kernel/test_ctype_k.c`

**Status:** ✅ Done (SCRUM-134)

**Full details:** [`docs/testing.md`](testing.md)

ExoDoom uses **KUnit** — a bare-metal CUnit-compatible test framework. When the
kernel is compiled with `-DTESTING`, `kernel_main` calls `run_tests()` instead
of the normal boot path. Tests run inside the kernel at boot time on real
hardware (or QEMU), so there is no host/target divergence.

`run_tests()` registers all suites, calls `CU_run_all_tests()`, and exits QEMU
via the `isa-debug-exit` device with code 0 (all pass) or 1 (any failure). The
GitHub Actions CI workflow greps the serial output for `ALL TESTS PASSED` and
fails the job if that string is absent or `TESTS FAILED` appears.

Current suites: `smoke` (harness self-check), `string` (13 tests for
`src/string.c`), `ctype` (5 tests for `src/ctype.c`).

New test files in `tests/kernel/` are picked up automatically by `build.sh` — no
Makefile changes needed.

---

## 9. Build and toolchain

**Files:** `Makefile`, `docker/Dockerfile.build`, `docker/Dockerfile.qemu`,
`docker/scripts/build.sh`

The entire toolchain runs inside Docker. No host cross-compiler is required.

| Image                     | Purpose                                                                   |
| ------------------------- | ------------------------------------------------------------------------- |
| `docker/Dockerfile.build` | Multi-stage build: compiles `x86_64-elf-binutils` 2.42 + `x86_64-elf-gcc` 13.2.0 from source, then creates a slim runtime image with GRUB ISO tools (`grub-mkrescue`, `grub-file`, `xorriso`) |
| `docker/Dockerfile.qemu`  | `qemu-system-x86_64` for running the ISO                                 |

**Build pipeline (`build.sh`):**

1. Assemble `src/boot.s` → `build/boot.o` (`x86_64-elf-as`)
2. Compile all `src/*.c` with `-std=gnu99 -ffreestanding -mno-red-zone -mcmodel=small -mno-sse -mno-sse2 -mno-mmx [-g -O0 | -O2]`
3. Assemble `src/isr.s` → `build/isr.o`
4. If `TESTING=1`: compile all `tests/kernel/*.c` with `-I src/`
5. Link everything with `src/linker.ld`, `-nostdlib`, `-z max-page-size=0x1000`, `-lgcc` → `build/exodoom` (ELF64)
6. Validate multiboot header with `grub-file --is-x86-multiboot2`
7. Stage ISO tree under `build/isodir/boot/`
8. `grub-mkrescue` → `build/exodoom.iso`

**Critical CFLAGS for x86_64 kernel code:**

- `-mno-red-zone` — mandatory; without it, interrupt handlers corrupt the 128-byte red zone below RSP
- `-mcmodel=small` — code/data assumed in lower 2 GB
- `-mno-sse -mno-sse2 -mno-mmx` — prevents GCC from emitting SIMD instructions **into kernel objects**, which is what makes it safe for the syscall and interrupt paths not to save XMM state (SCRUM-177; see `docs/syscall_spec.md` §3.4a — dropping this flag breaks that invariant). SSE itself *is* enabled in `boot.s` for the Doom objects, which are compiled without it by `docker/scripts/build-doom.sh`.

**Build-script details worth knowing before editing `build.sh`:**

- **File discovery is globbing.** Adding a `src/*.c` or `src/*.s` needs no
  build-script change. `src/doom/` is deliberately *not* globbed (see the
  vendored-engine notes in §7), and neither is any *subdirectory* of `src/` or
  `tests/kernel/` — that is the opt-out mechanism `src/libos_fs/` and the
  ring-3 link-target directories below all use. Under `TESTING=1`,
  `tests/kernel/*.c` is globbed the same way, so a new test file needs no
  build change either (`docs/testing.md`).
- **`-DEXO_KERNEL` is on the command line, not per file**, so it is
  guaranteed to precede every transitive include of `src/exo_syscall.h`. It
  selects the kernel view of that header (numbers, shared structs, error
  codes) over the LibOS view (user-side `syscall` stubs). Test TUs get it too;
  the one that needs the LibOS view (`tests/kernel/test_exo_syscall_k.c`)
  `#undef`s it first.
- **The kernel C compile also passes `-I src`** (SCRUM-74).
  `src/doomgeneric_exo.c` includes `doom/doomgeneric.h` for the `DG_*`
  prototypes rather than restating them, and that header's own
  `#include <stdlib.h>` only finds the shim's `src/stdlib.h` with `src/` on
  the angle-bracket path. `src/` holds no header that shadows one of GCC's
  freestanding four (`stddef`/`stdint`/`stdarg`/`limits`), so nothing is
  redirected that wasn't already coming from `src/`.
- **`boot.s` is assembled with its own flags**, and step 1b verifies the
  identity map's U/S protection matches the build type: `TESTING=1` passes
  `--defsym RING3_PROBE=1`, which sets the U/S bit at every paging level so
  the ring-3 syscall probes can run against the kernel map. All of physical
  memory is then reachable from CPL 3 — test builds only. `src/vmm.c`
  mirrors that gate with `#ifdef TESTING`; change one and change the other
  or a test build triple-faults (`docs/memory.md` §7).
- **Ring-3 link targets.** `build_ring3_link_target()` links compiled C so it
  runs at the addresses `libos_build_image()` really maps a LibOS to. It
  instantiates `tests/kernel/ring3_link_target.ld.in` per target, placing
  `.text` at `LIBOS_LAUNCH_CODE_VADDR` and `.data`/`.bss` at
  `LIBOS_LAUNCH_DATA_VADDR` (`src/libos_launch.h`) instead of the kernel's own
  2 MiB link address. Because the compiler and linker see the real address,
  ordinary compiled C works with no hand-written PIC convention — unlike the
  earlier hand-assembled `tests/kernel/*.s` ring-3 probes. `.text`/`.data` are
  extracted with `objcopy` and embedded in the kernel image as ordinary blobs
  (`_binary_<target>_{code,data}_bin_{start,end}`); `.bss` has no file bytes,
  so its length goes into a generated `<target>_layout.h` that lives inside the
  target's own directory so including it needs no extra `-I`. Targets today:
  - `tests/kernel/libos_c_probe/` (SCRUM-173) — one throwaway function, proving
    a real bound syscall (`exo_serial_write`) works from *compiled* ring-3
    code. Driven by `test_libos_c_probe_k.c`.
  - `tests/kernel/libc_shim_probe/` (SCRUM-51) — the *real* libc shim
    (`src/stdlib.c`, `stdio.c`, `string.c`, `ctype.c`, `libos_heap.c`,
    `libos_page_alloc.c`) compiled a second time **without** `-DEXO_KERNEL`.
    That flip is what SCRUM-51 did: the same `#ifdef EXO_KERNEL` switch
    `exo_syscall.h` uses now also selects `malloc`/`free`/`realloc`
    (`kmalloc` vs. `libos_heap_alloc`/`_free`/`_realloc`), `printf`'s sink
    (`serial_putc` vs. a buffered `exo_serial_write`) and
    `libos_page_alloc.c`'s page source (in-process `exo_syscall_dispatch()` vs.
    the real inline `syscall` stubs). `test_libc_shim_probe_k.c` launches it
    under `PAGE_OWNER_LIBOS` rather than a fresh id, because
    `exo_page_alloc`/`exo_page_map` resolve "the caller's address space"
    through `syscall_current_context()`, hardcoded in v1 to that one id. Both
    `LIBOS_LAUNCH_MAX_CODE_PAGES`/`_DATA_PAGES` grew (4/4 → 8/16) to fit it.
  - the demo LibOS apps and `libos_doom` (SCRUM-66), through the same
    function; `libos_doom` alone passes `-msse -msse2` via the `extra_cflags`
    parameter (§7).

  Entry-point and params placement rules for all of them are in
  `docs/libos-authoring.md` §4.
- **Toolchain fetches have more than one host.** Every `docker-*` target
  leads with the same `docker buildx build` of `docker/Dockerfile.build`, so a
  failure fetching the cross-toolchain sources fails all of them, with a
  `wget` error that says nothing about the tree being built.
  `docker/scripts/fetch-gnu-tarball.sh` tries ftp.gnu.org, then three
  independent GNU mirrors (plus sourceware.org for binutils), with a bounded
  20 s per host so a dead one costs seconds. **The sha256 pin in
  `Dockerfile.build` is not optional and lives next to the version `ARG` it
  belongs to** — fetching from more hosts means trusting more hosts, and the
  pin is also what rejects the truncated transfer or HTML error page a
  misbehaving mirror serves, at the download instead of as a baffling `tar`
  error minutes later. A version bump must update the checksum beside it.
- **CI** (`.github/workflows/ci.yml`) runs `make docker-ci` and greps serial
  output for `ALL TESTS PASSED` / `TESTS FAILED`; it also runs
  `make docker-build-doom` and `make docker-link-doom` (§7).

**Key make targets:**

| Target                          | What it does                                          |
| ------------------------------- | ----------------------------------------------------- |
| `make docker-build`             | Build kernel + ISO                                    |
| `make docker-run`               | Boot ISO in QEMU (GRUB menu)                          |
| `make docker-run-kernel`        | Boot the ISO with the GRUB menu skipped (`qemu -kernel` can't load Multiboot 2) |
| `make docker-test`              | Build with `TESTING=1`, boot, stream serial output    |
| `make docker-ci`                | As `docker-test`; used by GitHub Actions              |
| `make docker-build DEBUG=1`     | Debug build (`-g -O0`) for GDB sessions               |
| `make docker-run-debug DEBUG=1` | Boot QEMU frozen at start, GDB port 1234 exposed      |
| `make clean`                    | Remove `build/`                                       |

**Full debugging guide:** [`docs/debugging.md`](debugging.md)

---

## 10. Sprint roadmap

The first 13 sprints extracted from the Jira board, showing the overall arc
from bare-metal foundations to a playable game. The board has since moved to
an epic-based backlog (SCRUM-136 onward) rather than continuing to number
sprints — see the epic breakdown below the table for everything filed since.

| Sprint                                                  | Focus                                    | Key deliverables                                                                                                                                                                                                                                                                                                                                                                                                                            |
| ------------------------------------------------------- | ---------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Sprint 1: Memory + Timer**                            | Foundations                              | Multiboot 2 mmap ✅ SCRUM-6, bump allocator ✅, bitmap page allocator ✅ SCRUM-7, PIT/timer ✅ SCRUM-9, sleep ✅ SCRUM-10, string.h ✅ SCRUM-11, ctype.h ✅ SCRUM-12, KUnit ✅ SCRUM-133/134, PS/2 keyboard ✅ SCRUM-13/14/18, x86_64 migration ✅, error_stub bug fix ✅ SCRUM-135                                                                                                                                                                    |
| **Sprint 2: VMem + Input + libc**                       | Virtual memory + input                   | Paging on (kernel page tables from the PMM ✅ SCRUM-15), page fault handler ✅ SCRUM-17, framebuffer+WAD mapped ✅ SCRUM-16, keyboard ring buffer ✅ SCRUM-18, PS/2 mouse init ✅ SCRUM-19, `printf`→serial shim ✅ SCRUM-20                                                                                                                                                                                                                                                           |
| **Sprint 3: Heap+Mouse+Syscalls** _(20 Apr – 4 May)_    | Kernel heap + syscall gate               | First-fit heap allocator (`kmalloc`/`kfree`/`krealloc`) backed by PMM ✅ SCRUM-25, on-demand heap growth from the PMM ✅ SCRUM-26, 10K-block allocate/free stress + leak audit ✅ SCRUM-27, mouse packet decoding + delta accumulator ✅ SCRUM-28/29, `stdlib.h` wrappers (`malloc`/`free`/`realloc`, `atoi`, `abs`, `rand`/`srand`, `qsort`) ✅ SCRUM-30, `errno`/`assert`/`abort` ✅ SCRUM-31, `syscall`/`sysret` entry path via MSRs ✅ SCRUM-32, `exo_get_ticks` as first end-to-end syscall ✅ SCRUM-33                                                                                                                                               |
| **Sprint 4: LibOS mem+input+math** _(4 May – 18 May)_   | LibOS address space + remaining syscalls | `exo_page_alloc`/`exo_page_free`/`exo_page_map`/`exo_fb_acquire` in dispatcher ✅ SCRUM-34/35/36 (framebuffer mapping is `exo_fb_acquire` + `exo_page_map`, composed LibOS-side by `libos_fb_map()` — SCRUM-36; there is no separate `exo_fb_map` syscall despite that ticket's title), LibOS-side page allocator + heap ✅ SCRUM-37/38, `exo_get_key`/`exo_get_mouse_delta` syscalls ✅ SCRUM-39 (`exo_kbd_poll` (#6) was; `exo_mouse_poll` (#7) was declared in the ABI but had no bound handler, no PS/2 mouse controller init and no IRQ12 wiring at all despite this row and SCRUM-19/28/29 reading ✅ — the actual driver and syscall binding landed in SCRUM-52, `src/ps2_mouse.c`/`src/syscall_mouse.c`; see `docs/syscall_spec.md` §3.2 row 7), Doom keycode → PS/2 scancode translation ⬜ SCRUM-40 (still To Do), `math.h` (fixed-point sin/cos/tan/atan, `abs`, `floor`/`ceil`) ✅ SCRUM-41, `strcasecmp`/`strncasecmp` ✅ SCRUM-43, `FILE*` shim (`fopen`/`fclose`/`fread`/`fwrite`/`fseek`/`ftell`) backed by `exo_file_*` ✅ SCRUM-42 (read half landed with SCRUM-75, write half in its own change: `src/libos_fs/exo_file_stdio.c`, `src/stdio.c`), `exo_file_*` ✅ SCRUM-44 — rescoped from a kernel dispatcher to a LibOS descriptor library over ExoFS (`src/libos_fs/exo_file.h`, `docs/filesystem.md` §6d; #9–16 stay reserved and unbound)                     |
| **Sprint 5: LibOS struct+ring 3** _(18 May – 1 Jun)_    | Ring 0 → ring 3 transition               | GDT with ring 0 + ring 3 segments ✅ SCRUM-45, TSS for kernel stack on ring-3 exceptions ✅ SCRUM-46, boot LibOS in ring 3 via `iret` to user-mode entry point ✅ SCRUM-47, separate page directory per LibOS ✅ SCRUM-48, LibOS binary loading at fixed user-space address ✅ SCRUM-49, `libos_main()` entry framework ✅ SCRUM-50, `exo_serial_write` syscall ✅ SCRUM-50, separate LibOS link target for compiled ring-3 code ✅ SCRUM-173, port libc shim (`malloc`/`printf`) to use syscall stubs ✅ SCRUM-51                                                                                                                    |
| **Sprint 6: Syscall harden+Iso** _(1 Jun – 15 Jun)_     | Security + correctness                   | Syscall argument validation (bounds-check pointers, reject kernel addresses) ✅ SCRUM-54, test LibOS cannot read/write kernel memory (#PF, protection not not-present) ✅ SCRUM-55, test LibOS cannot execute IN/OUT instructions (#GP) ✅ SCRUM-56, consistent `exo_errno.h` error codes ✅ SCRUM-57, automated LibOS test suite via serial ✅ SCRUM-58, memory isolation stress test (allocate/free all pages, verify no kernel corruption) ⬜ SCRUM-59 (PR #110 open), syscall round-trip benchmarks ✅ SCRUM-60                                                        |
| **Sprint 7: Vendor Doomgeneric** _(15 Jun – 29 Jun)_    | Doom source integration                  | Vendor doomgeneric into `src/doom/` (or git submodule) ✅ SCRUM-63, resolve all compile errors (missing types, headers, signatures) ✅ SCRUM-64, fill remaining libc gaps found during compilation ✅ SCRUM-65, link Doom + LibOS + libc shim into single exodoom ELF ✅ SCRUM-66, `DG_Init` loading IWAD from multiboot module via the mapped WAD module (`mmap_find_module()`, SCRUM-16) ✅ SCRUM-73, `DG_GetTicksMs`/`DG_SleepMs` via `exo_get_ticks` ✅ SCRUM-74, `FILE*`-over-ExoFS IWAD reader replacing the memory-mapped one ✅ SCRUM-75 (`w_file_stdc.c` itself is the unmodified upstream reader; the change is the `fopen` backend and `DG_Init`, §7), `sscanf` with `%d %f %x %s` support ⬜ SCRUM-76 |
| **Sprint 8: DG** callbacks+render_* _(29 Jun – 13 Jul)_ | First pixels on screen                   | `DG_DrawFrame`: blit 640×400 ARGB → 1024×768 FB (scaled), nearest-neighbour integer scale with 32-bit writes, `DG_GetKey` wired to `exo_get_key` with Doom keycode translation, `i_input.c` patched to post `ev_mouse` via `exo_mouse_poll`, debug Doom startup crash sequence (`Z_Malloc`, `W_Init`, `R_Init`), stub `I_StartSound`/`I_StopSound`/`I_UpdateSound` as no-ops ✅ SCRUM-82 (no-op by configuration -- `FEATURE_SOUND` undefined -- plus a build gate that keeps it that way; superseded by SCRUM-101, which turns sound on and inverts that gate), stub `I_Error`/`I_Quit` to serial + halt ✅ SCRUM-83                      |
| **Sprint 9: Playability E1M1** _(13 Jul – 27 Jul)_      | Playable first level                     | Debug and fix E1M1 rendering (walls, floors, ceilings, sprites), verify combat (shooting, enemy AI, damage, pickups, status bar), menu navigation (new game, options, difficulty, quit), performance profiling (frame time per subsystem), fix top 3 bottlenecks, verify 35 tics/sec game loop timing, test with Freedoom2 IWAD and original DOOM2.WAD                                                                                      |
| **Sprint 10: Gameplay + Save/Load**                     | Save games                               | Save/load over ExoFS (`exo_file_*`, LibOS), `exo_file_remove`/`exo_file_rename` (SCRUM-106), `sscanf` for config                                                                                                                                                                                                                                                                                                                                                  |
| **Sprint 11: Sound + Storage**                          | Audio + persistence                      | ATA PIO driver, `exo_disk_read`/`exo_disk_write`, save file persistence across reboots, PC speaker driver, Doom SFX mapping                                                                                                                                                                                                                                                                                                                 |
| **Sprint 12: 2nd App + Context Switch**                 | Multitasking                             | Context table ✅, `CR3` swap ✅, `exo_yield` ✅, shell LibOS ✅, framebuffer multiplexing ✅ SCRUM-112, Ctrl+Tab hotkey ✅ SCRUM-111                                                                                                                                                                                                                                                                                                                                              |
| **Sprint 13: Harden + Compat Test**                     | Hardening                                | Regression suite ✅ SCRUM-114 (all-syscall, memory-isolation, input and timer coverage across `tests/kernel/`, `ExoDoom CI` now a required merge check), fuzz testing syscalls ✅ SCRUM-115 (`tests/kernel/test_syscall_fuzz_k.c`), test with DOOM.WAD / DOOM2.WAD / Freedoom2 ⬜ SCRUM-116, performance report ⬜ SCRUM-117, code cleanup ⬜ SCRUM-119                                                                                                                                                                                                                                                                                                       |

The board has since grown well past these original 13 sprints, organized
today under 16 epics (SCRUM-136–151) rather than dated sprint windows.
Everything below SCRUM-135 that isn't one of those epics is a story/task/bug
filed under one, so it is grouped by epic here instead of forcing it into a
sprint number that was never assigned:

| Epic | Status | What it actually covers |
| --- | --- | --- |
| SCRUM-151 Resource Protection & Secure Binding | To Do (epic), but its stories are almost entirely done | Page ownership table ✅ SCRUM-152, ownership enforcement in `exo_page_map`/`_unmap` ✅ SCRUM-153, framebuffer secure binding ✅ SCRUM-154, ownership-driven reclamation on `exo_exit` ✅ SCRUM-155, revocation protocol stub ✅ SCRUM-156, isolation test ✅ SCRUM-157, mapping teardown on free/revocation ✅ SCRUM-159, quota page-table pages per LibOS ✅ SCRUM-160; still open: manage all usable memory regions ⬜ SCRUM-158, `split_large_page` PAT-bit bug ⬜ SCRUM-161 |
| SCRUM-147 Multi-LibOS & Scheduling | To Do (epic), core mechanism done, hardening ongoing | Context table ✅ SCRUM-107, context switch ✅ SCRUM-108, `exo_yield` ✅ SCRUM-109, shell LibOS ✅ SCRUM-110, Ctrl+Tab hotkey ✅ SCRUM-111, FB multiplexing ✅ SCRUM-112; **in progress:** per-context FB binding + VA window replacing the single global ones — SCRUM-166; Ctrl+Tab's `context_current()`-flipped-before-the-real-switch race ✅ fixed for syscall attribution — SCRUM-179 (`context_switch_request()` no longer updates `context_current()` itself; `context_switch_tail` commits it after the real CR3 swap); SCRUM-180 (the same root cause, for `fb_compositor`'s foreground pick) likely resolved as a side effect but not yet verified/closed; still open: moving the compositor's FB copy out of `irq0_handler` — SCRUM-181, `swapgs`/per-CPU rework — SCRUM-176, generalized single-syscall launch dispatch — SCRUM-184, preemptive (stretch) — SCRUM-127 |
| SCRUM-142 Ring 3 & LibOS Runtime | **In Progress** (epic) | LibOS launch mechanism, entry convention, link target, and app-loading convention (SCRUM-47/48/49/50/51/173/175) are all done; the WAD/flat/automap viewer (SCRUM-165/178) and the demo-app line (clock ✅ SCRUM-168, Snake — **In Review**, SCRUM-182, calculator ⬜ SCRUM-169, Tetris ⬜ SCRUM-183) are what's currently exercising it |
| SCRUM-144/145 Doom Port & Gameplay Verification | In Progress (epic) | Doom is linked into `build/exodoom` as the `libos_doom` ring-3 target and playable from the shell's `doom` command (SCRUM-66/77/79) — this row used to say otherwise. Verified so far: the net layer, stubbed to single player with no uninitialised net state ✅ SCRUM-97 (§7 "Networking: stubbed, single player only", `tests/kernel/test_doom_net_k.c`). Still open: game-loop timing (SCRUM-89) and frame-time profiling (SCRUM-87) are in draft PRs, IWAD compatibility (SCRUM-90), and save/load + config persistence (SCRUM-92/93), which wait on the file layer (SCRUM-42/44) |
| SCRUM-146 Audio | In Progress | PC speaker driver ✅ SCRUM-98 (`src/speaker.c/h`, PIT channel 2, non-blocking duration via IRQ0); SFX→tone table ✅ SCRUM-99 (`src/doom_sfx_tone.c/h`, 1–4 freq/duration steps per `sfxenum_t` id); `exo_sound_tone`/`exo_sound_stop` syscalls ✅ SCRUM-100 (#17/#18, `src/syscall_sound.c/h`; ring-3 port access to the speaker proven to #GP); Doom `sound_module_t` ✅ SCRUM-101 (`src/doom_sound.c/h`, `FEATURE_SOUND` on, one-voice sequencer driven from `I_UpdateSound`, Doom priority rule; music is a silent module). All four stories done — `docs/syscall_spec.md` §6 Option B. **Real PCM audio has since started on top of that:** PCI enumeration + config space ✅ SCRUM-209 (`src/pci.c/h`, mechanism #1 over 0xCF8/0xCFC, `docs/drivers/pci.md`) and the Intel HDA controller driver ✅ SCRUM-210 (`src/hda.c/h`, CORB/RIRB codec ring + BDL stream DMA + INTx completion interrupt, `docs/drivers/hda.md`) — a synthesized tone now plays by DMA out of system memory, with register/DMA/IRQ state asserted by the `hda` KUnit suite. The other end of that path is now real too: the WAD DMX sound lump decoder ✅ SCRUM-211 (`src/doom_dmx.c/h`, `DS*` → 8-bit unsigned PCM, zero-copy out of the mounted module, 16 padding samples stripped at each end), acceptance-tested against the vendored `freedoom2.wad` by the `doom_dmx` suite. **That decode disproved the ticket's premise and the mixer must not inherit it:** DMX lumps are not "almost universally 11025 Hz" — of freedoom2 v0.13.0's 109 `DS*` lumps, 67 are 22050 Hz, 38 are 11025, 2 are 17990, 1 is 16000 and 1 is 44100, so with HDA pinned at 48 kHz/16-bit/stereo, resampling is unavoidable rather than optional. **And the two ends are now joined:** the software PCM mixer ✅ SCRUM-212 (`src/pcm_mixer.c/h`, 8 voices matching Doom's `snd_channels`, per-voice 16.16 phase accumulator with linear interpolation so each lump plays at *its* rate, int32 summation saturating to int16, Doom's own squared-separation panning law) plus a streaming mode in the driver (`hda_pcm_start()`, the BDL rediced from 2 entries to `HDA_BDL_ENTRIES_PCM` = 16 so a slice is ~21 ms rather than ~171 ms, refilled from the stream-completion IRQ) — `docs/drivers/hda.md` §12. Overlap and the non-disturbance of frame timing are both asserted from hardware state by the `hda_pcm` suite, the latter as a ratio between the PIT clock and the audio DMA rate, since timing a wait against the clock that gates it proves nothing; the arithmetic itself is asserted sample-for-sample by the hardware-free `pcm_mixer` suite. **And ring 3 can now reach it:** the PCM sound syscalls ✅ SCRUM-213 (`exo_sound_pcm` #28 / `exo_sound_pcm_stop` #29, `src/syscall_sound.c`), which resolved the story's own open design question by making sound **shared rather than exclusively bound** — there is no `exo_sound_acquire` and no `src/sound_binding.c`, because `pcm_mixer_start()`'s eight voices, Doom priority rule and non-disturbing refusal already are the admission policy a binding table would answer worse; what is owned is a voice, not the device (`docs/syscall_spec.md` §3.5b). The samples are copied into kernel pages rather than played in place: the mixer renders from `hda_irq_handler()` under whatever CR3 is loaded, and the caller may free its buffer the moment the syscall returns. A ring-3 LibOS queueing its own samples and the controller then DMA'ing them is asserted end to end by the `syscall_sound_pcm` suite. The ring-0 boot demo SCRUM-212 added to `kernel_main` is gone with it — the boot tone stays. **And Doom itself now plays through it:** the sound module's PCM leg ✅ SCRUM-214 (`src/doom_sound_pcm.c/h`, plus `pcm_mixer_set_params()` and `exo_sound_pcm_params` #30 — Doom retunes live sounds once a tic via `I_UpdateSoundParams`, which the mixer had no way to honour), so firing the shotgun in-game plays `DSSHOTGN` itself, the epic's own definition of done. **The two wirings coexist by a runtime rule rather than a build-time switch**, which the ticket asked for explicitly since only one can own `I_StartSound`: PCM is tried first and SCRUM-101's tone sequencer is the per-effect fallback — no WAD or no such lump costs that one effect, a `-EXO_ENODEV` (no audio controller) latches the whole path off for the run, and a `-EXO_EBUSY` counts as handled rather than failed, since the mixer declined that sound on purpose and a tone would answer it a second time (`docs/syscall_spec.md` §6 Option C). Nothing is copied LibOS-side: `LIBOS_WAD_VADDR` is inside the user window and #28 checks its buffer readable, so a decoded DMX lump is already a valid argument. The `doom_sound_pcm` suite asserts the shotgun's own first sample on the voice — 3072, the unity-gain conversion of DMX 140 — and then that those bytes leave RAM. Still open on that line: `src/revoke.c` has no sound leg (an exiting LibOS loses its voices, a revoked one does not), and music is still silent (`DG_music_module` is MUS/MIDI, not PCM). **The music path (SCRUM-215–220) has started from the mixer end:** a streaming music voice ✅ SCRUM-218 (`pcm_mixer_music_*` in `src/pcm_mixer.c/h` — one ring of 16-bit stereo at 48 kHz outside the sfx voice table, so no effect can steal or stop it; its own 0..127 level; an empty ring renders silence and counts an underrun rather than stalling the IRQ; `PCM_MIXER_MUSIC_PAGES` = 12, 256 ms; `docs/drivers/hda.md` §12.7). The MUS/MIDI song reader and sequencer ✅ SCRUM-215 (`src/libos_doom/doom_midi.c/h`, ring 3: sniffs `MThd` vs `MUS\x1a`, converts MUS with the vendored `mus2mid.c`, validates every track at open, merges format-1 tracks and yields channel events in exact integer microseconds with loop support; all 35 freedoom2 `D_*` lumps open and play through in the `doom_midi` suite; `docs/drivers/hda.md` §12.8). The GENMIDI instrument table decoder ✅ SCRUM-216 (`src/libos_doom/doom_genmidi.c/h`, ring 3: the IWAD's 175 OPL patches decoded by value out of the mounted lump, exact-size and magic checked, a missing lump reported as `ENOENT` rather than a fault; the `doom_genmidi` suite asserts freedoom2's records field by field; `docs/drivers/hda.md` §12.9). The OPL3 synth ✅ SCRUM-217 (`src/libos_doom/doom_opl.c/h` over vendored, integer-only Nuked-OPL3 in `src/nuked_opl3/`, ring 3: DMX's voice/frequency/volume behaviour on 18 OPL3 channels, steals the oldest voice, an exact 49716→48000 Hz rational resampler, exact digital silence after release; 1 s of `D_OPENIN` costs 169 ms under QEMU TCG, 4.8 ms per tic; `docs/drivers/hda.md` §12.10). The stream syscalls ✅ SCRUM-219 (`exo_sound_stream_write`/`_stop`/`_volume` #32–#34, `src/syscall_sound.c`: the music stream is **bound** to its first writer — `-EXO_EBUSY` to others, released by `exo_exit` — over a 12-page kernel ring; `write` never blocks and packs frames-accepted/room-left into its result; `docs/syscall_spec.md` §3.5c; `syscall_sound_stream` suite incl. a ring-3 probe). **And the music is audible:** the Doom wiring ✅ SCRUM-220 (`src/libos_doom/doom_sound_music.c/h`, ring 3, behind `DG_music_module` in `src/doom_sound.c`: `Poll` is now the per-frame hook that advances the song, renders OPL3 and fills the stream only as far as the ring has room, with the song clock counted in rendered frames so it cannot drift; pause drops the queued audio as well as the notes; any stream error latches music off for the run; `libos_doom` is 106 code / 112 data pages of 192 so the caps stand; `docs/drivers/hda.md` §12.11; `doom_sound_music` suite) |
| SCRUM-243 USB Support | In Progress (epic) | Controller topology + interrupt delivery spike ✅ SCRUM-245 (`docs/drivers/usb.md`): three UHCI controllers (`ich9-usb-uhci1..3`, 6 root ports, every device on an explicit `port=` so QEMU never adds a hub), **polled from IRQ0 with `USBINTR = 0`** because QEMU routes a UHCI function onto HDA's IRQ 11 and there is no shared-IRQ dispatch, no hub driver, and each `usb-kbd` bound to its own `secondary-vga` console with `display=` so it doesn't take keystrokes from PS/2. UHCI + enumeration + HID boot keyboard ✅ SCRUM-198 (under the dual-Doom epic SCRUM-195; `src/uhci.c`, `src/usb.c`, `src/usb_kbd.c`, usb.md §7–§12): a host-controller ops table the xHCI driver can reuse, one fixed schedule per controller, synchronous busy-waited control transfers on the controller's frame clock, and one event ring per keyboard. Two `usb-kbd`s typed into separately over QMP are told apart in CI (`usb` suite, `docker/scripts/qmp-keys.sh`). Still open: per-context routing of those rings ⬜ SCRUM-199, HID mouse ⬜ SCRUM-246, mass storage ⬜ SCRUM-247 (registers into the multi-disk block layer, ✅ SCRUM-244), xHCI (stretch) ⬜ SCRUM-248 |
| SCRUM-250 ACPI table parser | In Progress | Read-only RSDP/RSDT/XSDT/FADT parser ✅ (`src/acpi.c/h`, `docs/drivers/acpi.md`): feeds SCRUM-249's RTC the CMOS century register and the "no CMOS RTC" flag. Runs before `vmm_init` because the kernel map does not map firmware tables. No AML interpreter (uACPI is a later ticket). |
| SCRUM-249 Real-time clock | In Progress | CMOS RTC driver ✅ (`src/rtc.c/h`, polled, UIP wait + double-read, Status B honoured, century from the FADT via SCRUM-250 with a 20xx fallback, `-ENODEV` when ACPI reports no CMOS RTC) and `exo_rtc_read` ✅ (#35, `src/syscall_rtc.c/h`, unowned, user pointer validated like `exo_memstat`); the `clock` LibOS app now shows wall-clock UTC beside its uptime stopwatch. `docs/drivers/rtc.md`. Out of scope: setting the clock, alarms, IRQ8, time zones, libc `time()`. |
| SCRUM-143 Storage & File I/O | To Do (epic), the whole bottom half of the stack now done | ATA PIO driver ✅ SCRUM-102 (`src/ata.c/h`, polled; all four IDE positions since SCRUM-244); `exo_disk_read`/`exo_disk_write` syscalls ✅ SCRUM-103 (`src/syscall_disk.c/h`, #24/#25, sector-addressed, zero filesystem knowledge); disk ownership/binding ✅ SCRUM-188 (`src/disk_binding.c/h`, #26 `exo_disk_acquire`, one exclusive owner); ported FAT-like filesystem ✅ SCRUM-189 (`src/libos_fs/`, **LibOS-space, not kernel** — `docs/filesystem.md`; variable-length names, superblock, mkdir/rmdir/readdir, open/read/write/seek/unlink, 66 tests in `tests/kernel/test_exofs_k.c`). Note this **supersedes the in-kernel ramdisk** that `docs/syscall_spec.md` §4 used to recommend — see the note at the head of that section. Still open: FAT-formatted QEMU disk image ⬜ SCRUM-190 (which also retires the multiboot WAD module), `exo_file_*` descriptor API over ExoFS ✅ SCRUM-44 (LibOS library, not syscalls — `src/libos_fs/exo_file.c/h`, `tests/kernel/test_exo_file_k.c`), the `FILE*` shim over it ✅ SCRUM-42/75 (`src/libos_fs/exo_file_stdio.c`; Doom reads its IWAD through it from the program volume, bound read-only via `EXO_PKG_WANT_PROGRAM_RO`), save/config persistence ⬜ SCRUM-92/93/104/106, shell filesystem commands ✅ SCRUM-202 (`mkfs`/`mkdir`/`ls`/`cat`/`rm` in `src/shell/shell_main.c`, the first ring-3 ExoFS consumer, over its own scratch volume distinct from the IWAD's), per-program volumes ✅ SCRUM-237 (the disk binding is a table of non-overlapping sector extents, `exo_disk_acquire(base, count)`; a package manifest can request a volume slot, `exo_disk_extent` #37 reports it, and two copies of `libos_voldemo` run at once each on its own ExoFS volume — `docs/syscall_spec.md` §3.5a), multi-disk block layer ✅ SCRUM-244 (`src/blkdev.c/h` registry; `exo_disk_*`/`exo_diskstat` take a device index, device 0 = system disk; the binding is one extent per owner *per device*, so one context can hold two disks; `exofs_mount/format` take the device; the test boot attaches a second IDE drive, `build/exodoom_disk2.img`, and `tests/kernel/test_multidisk_k.c` copies an ExoFS file between the two — `docs/syscall_spec.md` §3.5a, `docs/drivers/ata.md`), the shell's `install` ✅ SCRUM-233 (`src/shell/shell_install.c`, device 1 → the system volume, `docs/filesystem.md` "Install"), shell `mount`/`umount` ✅ SCRUM-266 (`src/shell/shell_mount.c`: a table of directory → `diskN:volM` that the shell routes paths through, mounting the volumes a command needs together; `disk` lists volumes; `docs/filesystem.md` "Mounts"), handle-based ExoFS ✅ SCRUM-265 (`exofs_mount()` returns an `exofs_vol_t *` that every call takes; up to `EXOFS_MAX_VOLUMES` volumes at once; `tests/kernel/test_exofs_multivol_k.c`; `docs/filesystem.md` §8) |
| SCRUM-148 Testing, CI & Isolation | Mostly done | CI pipeline ✅ SCRUM-120 (Docker build + QEMU boot + serial suite on push, now a required status check on `main`); test harness ✅ SCRUM-58, regression suite ✅ SCRUM-114, syscall fuzzing ✅ SCRUM-115, syscall benchmarks ✅ SCRUM-60; still open: memory isolation stress test — SCRUM-59 (PR #110 open), IWAD compat suite — SCRUM-116 |
| SCRUM-149 Documentation & Delivery | Mostly to do, one piece done | README refresh ✅ SCRUM-167, CLAUDE.md split into onboarding + docs/ ✅ SCRUM-207; final architecture doc, contributor guide, release tag, demo video, client presentation, post-mortem (SCRUM-91, 119, 121–132) all still To Do |
| SCRUM-136/137/138/139/140/141 (Memory Mgmt / Interrupts & Timing / Kernel libc / Input / Graphics / Syscall ABI) | To Do (epics) | These are retroactive groupings over Sprints 1–5's already-✅ work above; their own epic tickets stay "To Do" because an epic isn't closed until every story under it is, and each still has open stories (e.g. SCRUM-40, SCRUM-158, SCRUM-161 above) |

Two small standalone fixes landed off the sprint grid entirely:
`fb_console` scroll doing a byte-at-a-time framebuffer copy ✅ SCRUM-162,
and the boot timer demo's ~6 ms/tick drift from sleeping relative instead of
to a deadline ✅ SCRUM-163. `qsort`'s O(n²) degradation on many-equal-key
input is also fixed ✅ SCRUM-171, and `KUNIT_MAX_SUITES` is due to grow again
from 48 to 256 (⬜ SCRUM-185) as more suites land.
