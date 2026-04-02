# ExoDoom Architecture

**Last updated:** 2 Apr 2026 — reflects Sprint 1 state

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
6. [Syscall interface](#6-syscall-interface)
7. [LibOS and doomgeneric](#7-libos-and-doomgeneric)
8. [Testing infrastructure](#8-testing-infrastructure)
9. [Build and toolchain](#9-build-and-toolchain)
10. [Sprint roadmap](#10-sprint-roadmap)

---

## 1. Project goal

ExoDoom is a bare-metal i386 exokernel whose primary goal is to run
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

```
┌──────────────────────────────────────────────────────┐
│                    Doom engine                       │  (unchanged doomgeneric source)
│   game logic · renderer · AI · physics · WAD I/O    │
├──────────────────────────────────────────────────────┤
│              doomgeneric platform shim               │  DG_Init, DG_DrawFrame, DG_GetKey …
├──────────────────────────────────────────────────────┤
│                  LibOS / libc shim                   │  malloc, printf, FILE*, math stubs
├──────────────────────────────────────────────────────┤
│            Exokernel  (ExoDoom kernel)               │  21 syscalls  ·  int 0x80
├────────────┬──────────┬──────────┬───────────────────┤
│  Physical  │  Timer   │  Input   │   Framebuffer     │  bare hardware
│  memory    │  (PIT)   │ PS/2 kbd │   (VESA/BGRX)     │
└────────────┴──────────┴──────────┴───────────────────┘
```

---

## 3. System layers

### Kernel (ring 0)

Written in freestanding C (gnu99) and x86 assembly. No libc. No external
dependencies beyond `libgcc` for compiler builtins. Responsible for:

- Physical memory management (page allocator, mmap parsing)
- Virtual memory (page tables, `CR3`)
- Interrupt handling (IDT, PIC remapping, IRQ dispatch)
- Hardware drivers: PIT, PS/2 keyboard, PS/2 mouse, serial (COM1), framebuffer
- The syscall gate (`int 0x80`) and all 21 `exo_*` syscall implementations
- A minimal ramdisk filesystem for config and save files

### LibOS (ring 3, planned)

A per-application user-space library linked directly into the application
binary. For the Doom LibOS this means:

- A bump/slab heap backed by `exo_page_alloc` calls
- A `FILE*` abstraction backed by `exo_file_*` syscalls (or a memory-mapped WAD
  shortcut)
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
GRUB 2  (grub.cfg: gfxpayload=keep, multiboot /boot/exodoom)
    │  loads kernel ELF at 2M, loads freedoom2.wad as multiboot module
    │  sets up framebuffer via VESA, passes multiboot_info* in %ebx
    ▼
_start  (src/boot.s)
    │  sets .set FLAGS = ALIGN|MEMINFO|VIDEO in multiboot header
    │  requests 1024×768×32 linear framebuffer from GRUB
    │  sets up 16 KiB stack, pushes %ebx (mb_info_addr), calls kernel_main
    ▼
kernel_main  (src/kernel.c)
    │
    ├─ serial_init()          — COM1 at 38400 baud, FIFO enabled
    ├─ mmap_init(mb)          — parse multiboot mmap, record usable/reserved regions
    ├─ memory_init()          — set bump allocator base to align_up(&_bss_end, 4K)
    │
    │  [if compiled with -DTESTING]
    ├─ run_tests()            — KUnit test runner, exits QEMU with pass/fail code
    │
    │  [normal boot]
    ├─ idt_init()             — fill all 256 IDT entries with default_stub, lidt
    ├─ pic_remap()            — remap PIC1→0x20, PIC2→0x28 (avoids BIOS conflict)
    ├─ idt_set_gate(32, irq0_stub) — wire IRQ0 to PIT handler
    ├─ pit_init(1000)         — PIT channel 0 at 1000 Hz (1 ms tick)
    ├─ sti                    — enable interrupts
    │
    └─ [framebuffer init, fb_console, LibOS launch — Sprint 2+]
```

Key details:

- The kernel is linked at **virtual address 2M** (`linker.ld`: `. = 2M`). This
  is also the physical load address pre-paging, so the initial identity mapping
  is trivial.
- GRUB passes a `multiboot_info*` struct in `%ebx`. `boot.s` pushes it as the
  single argument to `kernel_main` (i386 calling convention: first arg on
  stack).
- The multiboot header requests `mode_type=0` (linear framebuffer), 1024×768×32.
  GRUB picks a compatible mode and fills `mb->framebuffer_*` fields.
- The WAD file (`freedoom2.wad`) is declared as a GRUB module and is already
  mapped in physical memory by the time `kernel_main` runs. Its address and size
  are in the `multiboot_info` modules list.

---

## 5. Kernel subsystems

### 5.1 Memory

**Files:** `src/memory.c`, `src/memory.h`, `src/mmap.c`, `src/mmap.h`,
`src/multiboot.h`

**Current state (Sprint 1):** Two-phase design, partially complete.

**Phase 1 — Multiboot mmap parsing** ✅ Done (SCRUM-6): `mmap_init()` walks the
multiboot memory map entries and stores up to 32 `mmap_region_t` records (base,
length, type). Usable RAM regions are type 1 (`MULTIBOOT_MMAP_AVAILABLE`). On
QEMU `-m 256M` this yields a large usable region starting just above 1M.

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

**Future (Sprint 2):** Paging enabled (`CR0.PG`), kernel identity-mapped,
framebuffer and WAD mapped at fixed virtual addresses. `exo_page_alloc` /
`exo_page_map` / `exo_page_unmap` syscalls exposed to LibOS (SCRUM-15, -16,
-17).

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

The x86 IDT has 256 entries. ExoDoom initialises all of them during
`idt_init()`:

1. The current code segment selector is read from `%cs` at runtime rather than
   hard-coded — a GRUB-provided selector mismatch causes a triple fault on the
   first `iret`.
2. Every entry is filled with `default_stub` — a bare `iret` in `src/isr.s`.
   This is safe for most vectors but **not** for exception vectors that push an
   error code (see blocker below).
3. `idt_load()` executes `lidt` with the IDT pointer.

The 8259A PIC ships with IRQ vectors 0–15 mapped to CPU exception vectors 0–15,
which would conflict. `pic_remap()` reinitialises both PICs via ICW1–ICW4 to map
IRQ0–7 → vectors 0x20–0x27 and IRQ8–15 → 0x28–0x2F. All IRQs except IRQ0 (timer)
are then masked.

IRQ0 (the PIT timer) is routed through `irq0_stub` in `src/isr.s`, which saves
all registers with `pusha`, calls `irq0_handler()` in C, and restores them
before `iret`.

> ⚠️ **Active blocker (SCRUM-135, High):** CPU exception vectors 8, 10–14, 17,
> 21, 29, and 30 push a hardware error code onto the stack before transferring
> control. The current `default_stub` does a bare `iret`, which pops the error
> code as the return `%eip` and misaligns the stack — causing a triple fault.
> **Any page fault (vector 14) or GPF (vector 13) will hard-crash the machine**
> until a separate `error_stub` (which pops the error code before `iret`) is
> implemented and installed on these vectors. This must be resolved before
> paging work begins in Sprint 2.

**Planned dedicated handlers (Sprint 2+):**

| Vector | Exception                | Handler plan                                        |
| ------ | ------------------------ | --------------------------------------------------- |
| 13     | General Protection Fault | Serial diagnostic + halt                            |
| 14     | Page Fault               | Print CR2, error code, faulting EIP to serial; halt |
| 32     | IRQ0 / Timer             | `irq0_stub` → `irq0_handler` ✅ Done                |
| 33     | IRQ1 / Keyboard          | `irq1_stub` → `irq1_handler` (SCRUM-13)             |
| 44     | IRQ12 / Mouse            | `irq12_stub` → `irq12_handler` (SCRUM-19)           |
| 0x80   | Syscall gate             | `syscall_stub` → dispatch table (Sprint 3+)         |

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

**Future (Sprint 11):** PIT channel 2 will be used for PC speaker tone
generation (`exo_sound_tone`) without interfering with channel 0.

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

**Status:** Implemented, not yet wired into normal boot path (unreachable after
`qemu_exit(0)` in current `kernel_main`; will be activated Sprint 2+).

GRUB sets up a VESA linear framebuffer and passes its physical address, pitch,
width, height, and bpp in the `multiboot_info` struct. The empirically confirmed
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

**Future (Sprint 2):** `exo_fb_acquire()` will return the framebuffer's physical
address to the LibOS, which maps it into its own address space via
`exo_page_map`. `DG_DrawFrame` will blit the 640×400 RGBA8888 `DG_ScreenBuffer`
into this region (with format conversion, since Doom produces RGBA and the
hardware is BGRX). **Sprint 12:** Framebuffer multiplexing so multiple LibOS
apps each get a virtual framebuffer and the kernel manages which is displayed.

---

### 5.6 Keyboard

**Files:** `src/isr.s` (stub), `src/idt.c` (gate) — driver not yet in its own
file

**Status:** 🔄 In Progress (SCRUM-13, SCRUM-14)

The PS/2 keyboard controller maps to IRQ1 (IDT vector 33). When a key is pressed
or released, the controller raises IRQ1 and places a scan code (Set 1) in port
`0x60`. `irq1_handler` reads the byte from `0x60` and sends EOI.

SCRUM-13 implements the IRQ1 handler to read raw scancodes. SCRUM-14 implements
the Set 1 → key enum translation table, tracking make/break codes and modifier
state (shift, ctrl, alt). Sprint 2 adds a ring buffer (SCRUM-18) so rapid input
isn't dropped, and the `exo_kbd_poll(event_out)` syscall to expose the queue to
the LibOS.

PS/2 mouse (IRQ12, port `0x60`/`0x64`, 3-byte packets) follows in Sprint 2
(SCRUM-19) and feeds `exo_mouse_poll`.

---

## 6. Syscall interface

**Full specification:** [`docs/syscall-spec.md`](syscall-spec.md)

Syscalls use `int 0x80` with the syscall number in `EAX` and up to 5 arguments
in `EBX`, `ECX`, `EDX`, `ESI`, `EDI`. Return value in `EAX`; negative values are
error codes.

The 21 syscalls grouped by category:

| Category    | Syscalls                                                            | Sprint     |
| ----------- | ------------------------------------------------------------------- | ---------- |
| Memory      | `exo_page_alloc`, `exo_page_free`, `exo_page_map`, `exo_page_unmap` | Sprint 2–3 |
| Framebuffer | `exo_fb_acquire`                                                    | Sprint 2   |
| Timer       | `exo_get_ticks`                                                     | Sprint 3   |
| Input       | `exo_kbd_poll`, `exo_mouse_poll`                                    | Sprint 3   |
| Debug       | `exo_serial_write`                                                  | Sprint 3   |
| File I/O    | `exo_file_open/close/read/write/seek/stat/remove/rename`            | Sprint 4–5 |
| Sound       | `exo_sound_tone`, `exo_sound_stop`                                  | Sprint 11  |
| Scheduling  | `exo_yield`                                                         | Sprint 12  |
| Lifecycle   | `exo_exit`                                                          | Sprint 3   |

The syscall gate (`int 0x80` → dispatch table) is planned for Sprint 3 once
paging is stable and the LibOS address space exists.

---

## 7. LibOS and doomgeneric

**Status:** Not yet started — planned Sprint 3 onwards.

### WAD loading

`freedoom2.wad` is passed as a GRUB multiboot module. Its physical address and
size are available in `multiboot_info->mods_addr`. The recommended approach is a
**memory-mapped WAD reader**: `fopen("freedoom2.wad")` in the LibOS returns a
fake `FILE*` backed by a pointer into the module's mapped memory, and
`fread`/`fseek` operate as offset arithmetic over that region. This avoids
implementing any real file I/O for the game's largest data source.

### libc shim scope

doomgeneric requires 82 C source files' worth of standard library. The most
significant gaps to fill (beyond what's already implemented) are `strdup`,
`strcasecmp`, `strncasecmp`, `atoi`, `malloc`/`free`, `printf`/`vsnprintf`,
`sscanf`, and the full `FILE*` interface. See `docs/syscall-spec.md` §2 for the
complete audit.

### doomgeneric platform functions

| Function            | Backed by                                                |
| ------------------- | -------------------------------------------------------- |
| `DG_Init`           | `exo_fb_acquire` + `exo_page_map`                        |
| `DG_DrawFrame`      | BGRX blit from `DG_ScreenBuffer` into mapped framebuffer |
| `DG_SleepMs`        | Busy-wait loop on `exo_get_ticks`                        |
| `DG_GetTicksMs`     | `exo_get_ticks`                                          |
| `DG_GetKey`         | `exo_kbd_poll` dequeue                                   |
| `DG_SetWindowTitle` | No-op (or `exo_serial_write` for debug)                  |

### Multi-application (Sprint 12)

The eventual goal is **cooperative multitasking** between the Doom LibOS and a
minimal shell LibOS. The kernel maintains a context table (`SCRUM-107`): each
LibOS has its own page directory (`CR3`), saved register set, and framebuffer
region. `exo_yield` triggers a context switch (save registers + `CR3` swap). A
keyboard hotkey (Ctrl+Tab) swaps input focus and the active framebuffer.

---

## 8. Testing infrastructure

**Files:** `src/kunit.h`, `tests/kernel/kunit.c`, `tests/kernel/test_runner.c`,
`tests/kernel/test_smoke.c`, `tests/kernel/test_string_k.c`,
`tests/kernel/test_ctype_k.c`

**Status:** ✅ Done and in review (SCRUM-134)

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

| Image                            | Purpose                                                       |
| -------------------------------- | ------------------------------------------------------------- |
| `techiekeith/gcc-cross-i686-elf` | `i686-elf-gcc`, `i686-elf-as`, `i686-elf-ld` cross toolchain  |
| `docker/Dockerfile.build`        | Adds GRUB ISO tools (`grub-mkrescue`, `grub-file`, `xorriso`) |
| `docker/Dockerfile.qemu`         | `qemu-system-i386` for running the ISO                        |

**Build pipeline (`build.sh`):**

1. Assemble `src/boot.s` → `build/boot.o` (i686-elf-as)
2. Compile all `src/*.c` with `-std=gnu99 -ffreestanding [-g -O0 | -O2]`
3. Assemble `src/isr.s` → `build/isr.o`
4. If `TESTING=1`: compile all `tests/kernel/*.c` with `-I src/`
5. Link everything with `src/linker.ld`, `-nostdlib`, `-lgcc` → `build/exodoom`
6. Validate multiboot header with `grub-file --is-x86-multiboot`
7. Stage ISO tree under `build/isodir/boot/`
8. `grub-mkrescue` → `build/exodoom.iso`

**Key make targets:**

| Target                          | What it does                                          |
| ------------------------------- | ----------------------------------------------------- |
| `make docker-build`             | Build kernel + ISO                                    |
| `make docker-run`               | Boot ISO in QEMU (GRUB menu)                          |
| `make docker-run-kernel`        | Boot kernel directly (no GRUB menu, faster iteration) |
| `make docker-test`              | Build with `TESTING=1`, boot, stream serial output    |
| `make docker-ci`                | As `docker-test`; used by GitHub Actions              |
| `make docker-build DEBUG=1`     | Debug build (`-g -O0`) for GDB sessions               |
| `make docker-run-debug DEBUG=1` | Boot QEMU frozen at start, GDB port 1234 exposed      |
| `make clean`                    | Remove `build/`                                       |

**Full debugging guide:** [`docs/debugging.md`](debugging.md)

---

## 10. Sprint roadmap

All 13 sprints extracted from the Jira board, showing the overall arc from
bare-metal foundations to a playable game.

| Sprint                                                  | Focus                                    | Key deliverables                                                                                                                                                                                                                                                                                                                                                                                                                            |
| ------------------------------------------------------- | ---------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Sprint 1: Memory + Timer** _(current)_                | Foundations                              | Multiboot mmap ✅, bump allocator ✅, bitmap page allocator 🔄, PIT/timer ✅, sleep ✅, string.h ✅, ctype.h ✅, KUnit ✅, PS/2 IRQ1 🔄, error_stub bug fix ⬜                                                                                                                                                                                                                                                                              |
| **Sprint 2: VMem + Input + libc**                       | Virtual memory + input                   | Paging on (identity map), page fault handler, framebuffer+WAD mapped, keyboard ring buffer, PS/2 mouse init, `printf`→serial shim                                                                                                                                                                                                                                                                                                           |
| **Sprint 3: Heap+Mouse+Syscalls** _(20 Apr – 4 May)_    | Kernel heap + syscall gate               | First-fit heap allocator (`kmalloc`/`kfree`/`krealloc`) backed by PMM, mouse packet decoding + delta accumulator, `stdlib.h` wrappers (`malloc`/`free`/`realloc`, `atoi`, `abs`, `qsort`), `errno`/`assert`/`abort`, `int 0x80` trap gate in IDT, `exo_get_ticks` as first end-to-end syscall                                                                                                                                               |
| **Sprint 4: LibOS mem+input+math** _(4 May – 18 May)_   | LibOS address space + remaining syscalls | `exo_page_alloc`/`exo_page_free`/`exo_page_map`/`exo_fb_map` in dispatcher, LibOS-side page allocator + heap, `exo_get_key`/`exo_get_mouse_delta` syscalls, Doom keycode → PS/2 scancode translation, `math.h` (fixed-point sin/cos table, `abs`, `floor`/`ceil`), `FILE*` shim (`fopen`/`fclose`/`fread`/`fwrite`/`fseek`/`ftell`) backed by `exo_file_*`, `strcasecmp`/`strncasecmp`, `exo_file_*` kernel dispatcher                      |
| **Sprint 5: LibOS struct+ring 3** _(18 May – 1 Jun)_    | Ring 0 → ring 3 transition               | GDT with ring 0 + ring 3 segments, TSS for kernel stack on syscall entry, boot LibOS in ring 3 via `iret` to user-mode entry point, separate page directory per LibOS, LibOS binary loading at fixed user-space address, `libos_main()` entry framework, port libc shim to use syscall stubs, `exo_serial_write` syscall                                                                                                                    |
| **Sprint 6: Syscall harden+Iso** _(1 Jun – 15 Jun)_     | Security + correctness                   | Syscall argument validation (bounds-check pointers, reject kernel addresses), test LibOS cannot read/write kernel memory (GPF), test LibOS cannot execute IN/OUT instructions (GPF), consistent `exo_errno.h` error codes, automated LibOS test suite via serial, memory isolation stress test (allocate/free all pages, verify no kernel corruption), syscall round-trip benchmarks                                                        |
| **Sprint 7: Vendor Doomgeneric** _(15 Jun – 29 Jun)_    | Doom source integration                  | Vendor doomgeneric into `src/doom/` (or git submodule), resolve all compile errors (missing types, headers, signatures), fill remaining libc gaps found during compilation, link Doom + LibOS + libc shim into single exodoom ELF, `DG_Init` loading IWAD from multiboot module via `exo_fb_map`, `DG_GetTicksMs`/`DG_SleepMs` via `exo_get_ticks`, memory-mapped WAD reader replacing `w_file_stdc.c`, `sscanf` with `%d %f %x %s` support |
| **Sprint 8: DG** callbacks+render_* _(29 Jun – 13 Jul)_ | First pixels on screen                   | `DG_DrawFrame`: blit 640×400 ARGB → 1024×768 FB (scaled), nearest-neighbour integer scale with 32-bit writes, `DG_GetKey` wired to `exo_get_key` with Doom keycode translation, `i_input.c` patched to post `ev_mouse` via `exo_mouse_poll`, debug Doom startup crash sequence (`Z_Malloc`, `W_Init`, `R_Init`), stub `I_StartSound`/`I_StopSound`/`I_UpdateSound` as no-ops, stub `I_Error`/`I_Quit` to serial + halt                      |
| **Sprint 9: Playability E1M1** _(13 Jul – 27 Jul)_      | Playable first level                     | Debug and fix E1M1 rendering (walls, floors, ceilings, sprites), verify combat (shooting, enemy AI, damage, pickups, status bar), menu navigation (new game, options, difficulty, quit), performance profiling (frame time per subsystem), fix top 3 bottlenecks, verify 35 tics/sec game loop timing, test with Freedoom2 IWAD and original DOOM2.WAD                                                                                      |
| **Sprint 10: Gameplay + Save/Load**                     | Save games                               | Ramdisk save/load (`exo_file_*`), `exo_file_remove`/`exo_file_rename`, `sscanf` for config                                                                                                                                                                                                                                                                                                                                                  |
| **Sprint 11: Sound + Storage**                          | Audio + persistence                      | ATA PIO driver, `exo_disk_read`/`exo_disk_write`, save file persistence across reboots, PC speaker driver, Doom SFX mapping                                                                                                                                                                                                                                                                                                                 |
| **Sprint 12: 2nd App + Context Switch**                 | Multitasking                             | Context table, `CR3` swap, `exo_yield`, shell LibOS, Ctrl+Tab hotkey, framebuffer multiplexing                                                                                                                                                                                                                                                                                                                                              |
| **Sprint 13: Harden + Compat Test**                     | Hardening                                | Regression suite, fuzz testing syscalls, test with DOOM.WAD / DOOM2.WAD / Freedoom2, performance report, code cleanup                                                                                                                                                                                                                                                                                                                       |
