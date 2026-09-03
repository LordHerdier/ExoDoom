# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

ExoDoom is a bare-metal i386 **exokernel** whose goal is to run Doom (via
[doomgeneric](https://github.com/ozkl/doomgeneric)) directly on hardware/QEMU,
with no conventional OS underneath. The kernel exposes hardware resources
(physical pages, framebuffer, I/O) directly and pushes policy into a
user-space **LibOS**; Doom itself runs unmodified against a 6-function
platform interface plus a libc shim. See `docs/architecture.md` for the full
design and `docs/syscall_spec.md` for the syscall/libc audit — read these
before making non-trivial changes, they are kept up to date per sprint and
contain a lot of "why" that isn't in the code.

The kernel is written in freestanding C (`-std=gnu99 -ffreestanding`) and x86
assembly. No libc, no external dependencies beyond `libgcc`.

## Build / run / test commands

Everything runs in Docker — **no local cross-compiler is required** (just
Docker and GNU Make).

```bash
make docker-build              # Build kernel + ISO -> build/exodoom, build/exodoom.iso
make docker-run                # Build, then boot the ISO in QEMU (GRUB menu)
make docker-run-kernel         # Build, then boot the kernel directly (no GRUB) -- preferred for dev iteration
make docker-test               # Build with TESTING=1, boot, stream serial test output
make docker-ci                 # Same as docker-test; what CI runs
make docker-build DEBUG=1      # Unoptimized build (-g -O0) for GDB
make docker-run-debug DEBUG=1  # Boot QEMU frozen at start, GDB stub on port 1234
make clean                     # rm -rf build
```

- `docker/scripts/build.sh` runs inside the build container: assembles
  `boot.s`, compiles every `src/*.c`, assembles `isr.s`, links via
  `src/linker.ld` (`-nostdlib -lgcc`), validates the multiboot header with
  `grub-file`, then builds `build/exodoom.iso` with `grub-mkrescue`.
- Test sources in `tests/kernel/*.c` are picked up **automatically** by
  `build.sh` when `TESTING=1` — no Makefile/build-script changes needed to add
  a test file.
- CI (`.github/workflows/ci.yml`) runs `make docker-ci` and greps serial
  output for `ALL TESTS PASSED` / `TESTS FAILED`.
- QEMU shortcuts: `Ctrl+A` then `X` to exit; `Ctrl+A` then `C` for the QEMU
  monitor.
- Pressing Enter at the GRUB menu is currently broken — use
  `docker-run-kernel` to boot directly instead.

### Running a single/new test

Tests run **inside the kernel at boot**, not on the host — there's no way to
run just one test file; `make docker-test` always runs the full suite in one
QEMU boot. To add a test:

1. Add `tests/kernel/test_<name>_k.c` using the KUnit API (`src/kunit.h`,
   CUnit-compatible: `CU_ASSERT_EQUAL`, `CU_ASSERT_STRING_EQUAL`, etc. — see
   `docs/testing.md` for the full macro list).
2. Register the new suite in `tests/kernel/test_runner.c`'s `run_tests()` via
   `CU_add_suite(...)` + your `suite_*_tests(s)` function.
3. `make docker-test` to run it.

Full guide: `docs/testing.md`.

### Debugging

`make docker-run-debug DEBUG=1`, then in a second terminal:
`gdb build/exodoom` → `set architecture i386` → `target remote localhost:1234`.
Full command reference and bare-metal-specific tips (framebuffer byte order,
`-O2` stepping caveats, `isa-debug-exit` requirements) in `docs/debugging.md`.

## Architecture

### Boot flow

`GRUB` (multiboot, loads `freedoom2.wad` as a module, requests a VESA
framebuffer) → `_start` in `src/boot.s` (sets up a 16 KiB stack, pushes
`mb_info_addr`) → `kernel_main` in `src/kernel.c`:

```
serial_init() -> mmap_init(mb) -> memory_init()
  [if -DTESTING]  run_tests() -> qemu_exit(pass/fail)
  [normal boot]   idt_init() -> pic_remap() -> wire IRQ0/IRQ1 gates
                  -> pit_init(1000) -> sti -> (fb/console init, WIP)
```

The kernel links at virtual/physical `2M` (`src/linker.ld`), so pre-paging
there is no virtual/physical distinction — this simplifies everything until
Sprint 2 paging work lands.

### Subsystem map

| Concern | Files |
|---|---|
| Boot / entry | `src/boot.s`, `src/linker.ld`, `src/multiboot.h`, `src/grub.cfg` |
| Memory (mmap parse, bump allocator) | `src/mmap.c/h`, `src/memory.c/h` |
| Interrupts (IDT/PIC/ISR) | `src/idt.c/h`, `src/pic.c/h`, `src/isr.s`, `src/io.h` |
| Timer (PIT) | `src/pit.c/h`, `src/sleep.c/h` |
| Serial (COM1, all diagnostic + test output) | `src/serial.c/h` |
| Framebuffer + text console | `src/fb.c/h`, `src/fb_console.c/h` |
| Keyboard (PS/2) | `src/ps2.c/h` |
| Freestanding libc bits | `src/string.c/h`, `src/ctype.c/h` |
| Test framework | `src/kunit.h`, `tests/kernel/*.c` |

### Key architectural facts worth knowing before editing

- **No paging yet.** All physical == virtual. `alloc_page`/`vmm_init`/syscall
  gate are Sprint 2+ work — see `docs/memory.md` and `docs/architecture.md`
  §5.1/§6 for the planned design before implementing anything in that space.
- **SCRUM-135 (error-code IDT vectors) resolved on `feat/x64`:** `default_stub`
  in `src/isr.s` used to do a bare `iretq` without popping the hardware error
  code that vectors 8, 10–14, 17, 21, 29, 30 push, which triple-faulted the
  machine on any page fault or GPF. A dedicated `error_stub` is now installed
  on those vectors in `idt_init()` and correctly discards the error code
  before `iretq`. See `docs/drivers/idt.md` §5/§7 for details.
- **Allocator today is a one-way bump allocator** (`memory_init`/`kmalloc` in
  `src/memory.c`) — no `free`, always 4K-aligned, used only for permanent
  early-boot kernel structures (IDT, future PMM bitmap, page tables). It is
  intentionally retired once the page allocator/paging exist.
- **COM1 serial is the only diagnostic/test output channel** right now
  (`src/serial.c`, mapped to QEMU stdio via `-serial mon:stdio`). Test framework
  output and all kernel diagnostics go through it; `serial_flush()` must be
  called before `qemu_exit()` or buffered bytes are lost.
- **Syscall interface (`syscall` instruction, 21 `exo_*` syscalls) is fully
  specified but mostly unimplemented** — `docs/syscall_spec.md` §3 and its C
  expression `src/exo_syscall.h` are the source of truth for what each syscall
  must do and which doomgeneric/libc call sites need it; change one and change
  the other. Consult them before adding a new syscall or libc shim function so
  the implementation matches the intended calling convention (`RAX`=number,
  `RDI/RSI/RDX/R10/R8/R9`=args, return in `RAX`, negative=error). The 4th
  argument is in `R10` rather than the SysV `RCX` because `syscall` itself
  overwrites `RCX` with the return RIP and `R11` with RFLAGS.
- **Framebuffer pixel format is BGRX8888** (empirically confirmed on QEMU),
  not RGB — relevant to anything touching `src/fb.c` or blit code.
- Sprint status/roadmap and current in-flight Jira stories are tracked in
  `docs/architecture.md` §10 — check it for what's actually in progress vs.
  planned before assuming a subsystem is finished.
