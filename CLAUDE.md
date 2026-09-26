# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

ExoDoom is a bare-metal x86_64 (long mode) **exokernel** whose goal is to run
Doom (via [doomgeneric](https://github.com/ozkl/doomgeneric)) directly on
hardware/QEMU, with no conventional OS underneath. The kernel exposes hardware
resources (physical pages, framebuffer, I/O) directly and pushes policy into a
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
make docker-build-doom         # Compile-only gate over vendored src/doom/ (79/79; CI runs it)
make docker-build DEBUG=1      # Unoptimized build (-g -O0) for GDB
make docker-run-debug DEBUG=1  # Boot QEMU frozen at start, GDB stub on port 1234
make clean                     # rm -rf build
```

- `docker/scripts/build.sh` runs inside the build container: assembles
  `boot.s` (with its own flags), **verifies the identity map's U/S protection
  matches the build type** (step 1b — see the RING3_PROBE note below), compiles
  every `src/*.c` with `-DEXO_KERNEL`, assembles every other `src/*.s`, links
  via `src/linker.ld` (`-nostdlib -lgcc`), validates the multiboot2 header with
  `grub-file`, then builds `build/exodoom.iso` with `grub-mkrescue`.
- Adding a `src/*.c` or `src/*.s` needs **no build-script change** — both are
  globbed. `src/doom/` is deliberately *not* globbed; see the vendoring note
  below.
- `-DEXO_KERNEL` is passed on the command line, not per-file, so it is
  guaranteed to precede every transitive include of `src/exo_syscall.h`. It
  selects the kernel view (numbers, shared structs, error codes) over the LibOS
  view (user-side `syscall` stubs). Test TUs get it too; the one that needs the
  LibOS view (`tests/kernel/test_exo_syscall_k.c`) `#undef`s it first.
- The kernel C compile also passes **`-I src`** (SCRUM-74). Every other
  `src/*.c` reaches its headers with quoted includes and is unaffected; the
  flag is there because `src/doomgeneric_exo.c` includes
  `doom/doomgeneric.h` for the `DG_*` prototypes rather than restating them,
  and that header's own `#include <stdlib.h>` only finds the shim's
  `src/stdlib.h` with `src/` on the angle-bracket path. `src/` holds no header
  that shadows one of GCC's freestanding four (`stddef`/`stdint`/`stdarg`/
  `limits`), so nothing is redirected that wasn't already coming from `src/`.
- Test sources in `tests/kernel/*.c` are picked up **automatically** by
  `build.sh` when `TESTING=1` — no Makefile/build-script changes needed to add
  a test file, and that loop's own invocation stays untouched no matter what
  gets added under `tests/kernel/`. `tests/kernel/libos_c_probe/` (SCRUM-173)
  is how a file opts out of it instead of the loop special-casing a filename:
  it lives in its own subdirectory, which `tests/kernel/*.c` never matches,
  and is built by its own explicit step. That step compiles and links
  `libos_c_probe.c` *separately*, via the shared
  `tests/kernel/ring3_link_target.ld.in` template (parameterized per target
  by `build_ring3_link_target()` in `build.sh` — see its own comment) that
  places `.text` at `LIBOS_LAUNCH_CODE_VADDR` and
  `.data`/`.bss` at `LIBOS_LAUNCH_DATA_VADDR` (`src/libos_launch.h`) — the real
  addresses `libos_build_image()` maps a LibOS to — rather than at the
  kernel's own 2M link address. Because the compiler/linker see the address
  this code will actually run at, ordinary compiled C works there with no
  hand-written PIC convention, unlike every earlier ring-3 probe in
  `tests/kernel/*.s`. The resulting `.text`/`.data` bytes are extracted with
  `objcopy` and embedded into the kernel image as ordinary blobs
  (`_binary_libos_c_probe_{code,data}_bin_{start,end}`); `.bss`'s length (no
  file bytes to extract) is written to a generated
  `tests/kernel/libos_c_probe/libos_c_probe_layout.h` instead — inside that
  same subdirectory so `#include`ing it from `tests/kernel/test_libos_c_probe_k.c`
  needs no extra `-I` flag either. `test_libos_c_probe_k.c` drives
  `libos_build_image()`/`libos_enter()` against those blobs the same way
  `test_libos_main_k.c` drives the hand-assembled probe, proving a real bound
  syscall (`exo_serial_write`) works from *compiled* ring-3 code — the
  prerequisite SCRUM-51's libc-shim port needs and didn't have before this.
- `tests/kernel/libc_shim_probe/` (SCRUM-51) is a second, independent
  application of that same link-target mechanism, opting out of the
  `tests/kernel/*.c` loop the same way and for the same reason. Where
  `libos_c_probe` links one throwaway probe function, this target links the
  *real* libc shim — `src/stdlib.c`, `src/stdio.c`, `src/string.c`,
  `src/ctype.c`, plus `src/libos_heap.c`/`src/libos_page_alloc.c` — compiled
  a second time, **without** `-DEXO_KERNEL` (every other `src/*.c` compile in
  `build.sh` always passes it). That flip is what SCRUM-51 actually did: it
  is the same `#ifdef EXO_KERNEL` switch `src/exo_syscall.h` already used to
  pick the kernel vs. LibOS view of the syscall ABI, now also gating
  `malloc`/`free`/`realloc` (kmalloc vs. `libos_heap_alloc`/`_free`/`_realloc`),
  `printf`'s sink (`serial_putc` vs. a buffered `exo_serial_write`), and
  `libos_page_alloc.c`'s page source (the in-process `exo_syscall_dispatch()`
  call its own kernel-side unit tests use vs. the real inline `syscall`
  stubs) on which side of that `#ifdef` a given compile is on. `libc_shim_probe.c`
  **must be linked first** on the object list: `libos_build_image()` always
  treats the base of the code blob (`LIBOS_LAUNCH_CODE_VADDR`) as the entry
  point rather than looking up a symbol, so whichever object file is linked
  first is what ends up there — for `libos_c_probe` this was automatic (only
  one object file); here it is an explicit ordering requirement documented
  at `build_ring3_link_target()`'s own comment in `build.sh` and at its
  `libc_shim_probe` call site, which lists `libc_shim_probe.c` first.
  `test_libc_shim_probe_k.c` launches
  it under `PAGE_OWNER_LIBOS` rather than a fresh per-suite id, because
  `exo_page_alloc`/`exo_page_map` (which `malloc` now reaches for real) route
  "the caller's address space" through `syscall_current_context()`, hardcoded
  in v1 to that one id — see that test file's own comment, and
  `LIBOS_LAUNCH_MAX_CODE_PAGES`/`_DATA_PAGES`'s SCRUM-51 comment in
  `src/libos_launch.h` for why both grew (4/4 → 8/16 pages) to fit a real
  libc shim rather than a one-function probe.
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
`gdb build/exodoom` → `set architecture i386:x86-64` →
`target remote localhost:1234`.
Full command reference and bare-metal-specific tips (framebuffer byte order,
`-O2` stepping caveats, `isa-debug-exit` requirements) in `docs/debugging.md`.

## Architecture

### Boot flow

`GRUB` (multiboot, loads `freedoom2.wad` as a module, requests a VESA
framebuffer) → `_start` in `src/boot.s` (sets up a 16 KiB stack, pushes
`mb_info_addr`) → `kernel_main` in `src/kernel.c`:

```
serial_init() -> framebuffer tag discovery (serial diagnostics only)
  -> idt_init() -> tss_init() -> mmap_init(mb) -> memory_init()
  -> page_alloc_init(mb) -> vmm_init(mb, fb)
  -> syscall_init() -> syscall_mem_init() -> syscall_fb_init(fb)
  -> syscall_serial_init()
  -> pic_remap() -> idt_set_gate(32, irq0_stub) -> pit_init(1000)
  -> syscall_pit_init()
  [if -DTESTING]  serial_flush() -> qemu_exit(run_tests())
  [normal boot]   fb_init_bgrx8888() + fbcon_init()  (halts if absent)
                  -> banner/mmap dump -> ownership self-check
                  -> idt_set_gate(33, irq1_stub) -> kbd_init()
                  -> sti -> `sti; hlt` idle loop
```

`idt_init()`/`tss_init()`/`pic_remap()`/`idt_set_gate(32, irq0_stub)`/
`pit_init(1000)`/`syscall_pit_init()` all run ahead of the `TESTING` branch
(SCRUM-172, SCRUM-51) — only IRQ1/keyboard wiring
(`idt_set_gate(33, irq1_stub)`, `kbd_init()`, `pic_unmask_irq(1)`) stays in
the normal-boot tail, since nothing under `TESTING` touches the keyboard and
`pic_remap()` leaves IRQ1 masked at the PIC precisely so a stray one arriving
before `kbd_init()` runs cannot reach `idt_init()`'s `default_stub`.

**Anything the ring-3 tests need must be initialised before the `TESTING`
branch** — that branch exits QEMU and never returns, so `page_alloc_init`,
`vmm_init` and the syscall `*_init()`s sit above it deliberately (the ring-3
probe runs against the page tables `vmm_init` installs). The framebuffer, IDT,
PIC, PIT and keyboard are all below it and do **not** exist during tests.

The kernel links at virtual/physical `2M` (`src/linker.ld`) and both the boot
map and the kernel map are identity maps, so virtual == physical throughout.
Per-LibOS address spaces can now be built (SCRUM-48), the mechanism to
actually launch one in ring 3 on its own address space exists and is tested
(SCRUM-47, `src/libos_launch.c/h` + `src/libos_enter.s`), and that mechanism
now places code and data at their own defined, fixed virtual addresses rather
than one undifferentiated blob (SCRUM-49) — see below. The `libos_main()`
entry convention (SCRUM-50) — code runs PIC at `LIBOS_LAUNCH_CODE_VADDR`,
references its own data only via the fixed immediate `LIBOS_LAUNCH_DATA_VADDR`,
and can call any bound syscall through the ordinary `exo_syscall.h` stubs —
is now proven end to end (`tests/kernel/libos_main_probe.s` calls the real,
bound `exo_serial_write`), but v1's one LibOS still runs on the kernel's own
map on a normal boot: nothing yet loads a real binary's code/data into that
convention and calls it from `kernel_main` instead of a test harness.

### Subsystem map

| Concern | Files |
|---|---|
| Boot / entry | `src/boot.s`, `src/linker.ld`, `src/multiboot2.h`, `src/grub.cfg` |
| Memory (mmap parse, bump allocator, bitmap PMM) | `src/mmap.c/h`, `src/memory.c/h`, `src/page_alloc.c/h` |
| Virtual memory (kernel page tables, map/unmap/translate) | `src/vmm.c/h` |
| Ring-3 LibOS launch (image build, CR3 switch, `iretq`) | `src/libos_launch.c/h`, `src/libos_enter.s` |
| Process/context table (id, page dir, saved registers, state) | `src/context.c/h` |
| Interrupts (IDT/PIC/ISR, TSS, page-fault diagnostics) | `src/idt.c/h`, `src/pic.c/h`, `src/isr.s`, `src/io.h`, `src/tss.c/h`, `src/fault.c/h` |
| Timer (PIT) | `src/pit.c/h`, `src/sleep.c/h` |
| PCI bus (config space, enumeration, BARs, SCRUM-209) | `src/pci.c/h` |
| Intel HDA audio (CORB/RIRB, BDL stream DMA, INTx, SCRUM-210) | `src/hda.c/h` |
| PC speaker (PIT channel 2, SCRUM-98) | `src/speaker.c/h` |
| Doom SFX → speaker tone table (SCRUM-99) | `src/doom_sfx_tone.c/h` |
| Doom `sound_module_t` over the speaker (SCRUM-101) | `src/doom_sound.c/h` (+ `FEATURE_SOUND` in `src/doom/doomfeatures.h`) |
| Serial (COM1, all diagnostic + test output) | `src/serial.c/h` |
| Framebuffer + text console | `src/fb.c/h`, `src/fb_console.c/h` |
| Syscall gate (entry, dispatch, handlers) | `src/syscall.c/h`, `src/syscall_entry.s`, `src/syscall_mem.c/h`, `src/syscall_fb.c/h`, `src/syscall_serial.c/h`, `src/syscall_sound.c/h` (SCRUM-100) |
| Resource ownership (secure binding) | `src/page_alloc.c/h` (pages), `src/fb_binding.c/h` (framebuffer), `src/disk_binding.c/h` (disk, SCRUM-188) |
| Resource revocation (repossession) | `src/revoke.c/h` (protocol), the `page_revoke_*`/`fb_binding_revoke_*`/`disk_binding_revoke_*` primitives |
| Keyboard (PS/2 + event ring) | `src/ps2.c/h`, `src/kbd_ring.c/h` |
| Freestanding libc bits | `src/string.c/h`, `src/ctype.c/h`, `src/stdio.c/h`, `src/stdlib.c/h`, `src/errno.c/h`, `src/fpconv.c/h` (float↔decimal); header-only: `src/strings.h`, `src/inttypes.h`, `src/math.h`, `src/unistd.h`, `src/assert.h`, `src/fcntl.h`, `src/sys/types.h`, `src/sys/stat.h` |
| Fixed-point trigonometry (sin/cos/tan/atan, integer floor/ceil) | `src/fixed_math.c/h` |
| Doom globals from non-vendored netcode | `src/doom_net_stub.c` |
| Vendored Doom engine (linked as the `libos_doom` ring-3 target, SCRUM-66) | `src/doom/`, `src/libos_doom/` |
| doomgeneric platform layer | `src/doomgeneric_exo.c/h` (timer half: `DG_GetTicksMs`/`DG_SleepMs`, SCRUM-74) |
| Doom fatal-error path (`I_Error`/`I_Quit` back end) | `src/doom_panic.c/h` |
| doomgeneric platform layer | `src/doomgeneric_exo.c/h` (`DG_Init` SCRUM-73; timer half `DG_GetTicksMs`/`DG_SleepMs`, SCRUM-74) |
| Mounted IWAD registry (behind `DG_Init`) | `src/doom_wad.c/h` |
| Test framework | `src/kunit.h`, `tests/kernel/*.c`, `tests/kernel/kunit.c`, `tests/kernel/ring3_probe.s` |

### Key architectural facts worth knowing before editing

- **Paging is on, and a LibOS can now get its own address space — but v1's one
  LibOS doesn't run in one yet.** `src/boot.s` builds a static 4 GB identity
  map to reach long mode; `vmm_init()` (`src/vmm.c`, SCRUM-15) then builds the
  real kernel map from PMM pages and loads CR3 with it, identity-mapping only
  what exists — low memory, kernel image + bump pool, usable RAM, the
  multiboot info and the framebuffer aperture. Virtual == physical everywhere,
  so no translation is ever surprising, but **page 0 is deliberately
  unmapped** as a NULL guard. `vmm_map_page`/`vmm_unmap_page`/`vmm_translate`
  are the primitives SCRUM-16 builds on and what `exo_page_map`/`exo_page_unmap`
  (SCRUM-35) are implemented in terms of; a 4 KiB map inside a 2 MB leaf splits
  it automatically. `vmm_create_address_space`/`vmm_destroy_address_space`/
  `vmm_switch_address_space` (SCRUM-48) build a real per-LibOS PML4 — sharing
  the kernel's own PML4[0] link so kernel memory needs no synchronization
  between address spaces — and a `page_owner_t`-keyed registry
  (`vmm_bind_address_space`/`vmm_address_space_for`) tracks which context runs
  on which root; `syscall_mem.c`'s `exo_page_map`/`exo_page_unmap` already
  resolve the caller's root through it. `src/libos_launch.c/h` +
  `src/libos_enter.s` (SCRUM-47, extended by SCRUM-49) are the real launch
  mechanism on top of all that: `libos_build_image()` builds a fresh address
  space and places code, data+bss and a stack at three fixed addresses inside
  the LibOS window — `LIBOS_LAUNCH_CODE_VADDR`/`_DATA_VADDR`/`_STACK_VADDR`
  in `src/libos_launch.h`. Code and data+bss each span up to
  `LIBOS_LAUNCH_MAX_{CODE,DATA}_PAGES` pages now, rather than the single
  undifferentiated page SCRUM-47 shipped with; the stack is still exactly one
  fixed page — and `libos_enter()` switches
  CR3 and `iretq`s to CPL 3 — proven by `tests/kernel/test_libos_launch_k.c`
  against a real, separate address space rather than the TESTING-only
  blanket-user-accessible trick `tests/kernel/ring3_probe.s` uses. This is
  still a fixed-size loader, not a general one: `code`/`data` are copied
  verbatim into position, position-independent by convention (no ELF, no
  relocation), and oversized input is rejected as `VMM_EINVAL` before
  anything is allocated. SCRUM-50 defines and proves the `libos_main()` entry
  convention on top: code launched at `LIBOS_LAUNCH_CODE_VADDR` is PIC by the
  same rule as `libos_build_image()`'s `code`/`data` (no internal jump/call,
  no RIP-relative reference; a reference to its own data uses the fixed
  immediate `LIBOS_LAUNCH_DATA_VADDR`), and it can call any syscall the
  dispatcher has bound through the ordinary `exo_syscall.h` stubs — proven by
  `tests/kernel/libos_main_probe.s` + `test_libos_main_k.c`, which call the
  real `exo_serial_write` (#8, `src/syscall_serial.c`, also SCRUM-50) from
  ring 3 and check the genuine syscall result comes back through
  `libos_return()`, not a fixed marker. What neither SCRUM-49 nor SCRUM-50
  does is wire this into `kernel_main`: on a normal boot, `kernel_main` still
  binds the one v1 LibOS to `vmm_kernel_pml4()` itself, a placeholder that
  keeps it running on the kernel's own map, because there is still no real
  binary's code/data to build an image from and the normal boot tail is a
  live interactive demo, not a placeholder waiting to be replaced. Read
  `docs/memory.md` §7, `docs/syscall_spec.md` §3.7 and `docs/architecture.md`
  §5.1/§6 before implementing anything in that space.
- **SCRUM-175 generalizes two conventions every ring-3 LibOS app (the shell,
  the WAD viewer, and every future one — SCRUM-168/169's clock/calculator,
  eventually SCRUM-110's shell extensions) now follows, rather than each
  reinventing its own.** First, entry-point placement:
  `__attribute__((section(".text.entry")))` on the target's real entry
  function, matched by `*(.text.entry)` placed first in
  `tests/kernel/ring3_link_target.ld.in`'s `.text` block, is *the* way to
  guarantee a target lands at `.text` offset 0 (`libos_build_image()`'s
  `entry_vaddr`) — "list the entry file first" alone breaks the moment a
  target has `static` helpers ahead of its entry point in source order,
  because at `-O0` GCC does not inline `exo_syscall.h`'s `static inline`
  syscall stubs away and they get emitted as real functions first. Second,
  argument hand-off: a LibOS app declares its own params struct as literally
  the first global in its own `.data` (non-zero-initialized, so it lands in
  `.data` and not `.bss` — GCC never stores real bytes for a zero
  initializer), and the kernel-side launcher calls
  `libos_launch_patch_params()` (`src/libos_launch.h`/`.c`) to overwrite it
  in place through `img->data_paddrs[0]` — a physical page the kernel
  already has identity-mapped access to — right after `libos_build_image()`
  succeeds. Both conventions depend on the same requirement: the target's
  entry-point-and-params TU must be linked first in
  `build_ring3_link_target()`'s source list, since GCC emits a TU's globals
  into `.data` in source order and `ld`'s `*(.data)` rule then collects
  input sections in link order. `src/libos_wad_viewer/libos_wad_viewer.c`'s
  `g_wad_params` + `src/syscall_launch.c`'s `sys_launch_wad_viewer()` is the
  worked example — it replaced an earlier, WAD-viewer-specific side channel
  (a second hand-mapped page at its own fixed VA) with this general pattern.
- **`vmm.c` mirrors boot.s's ring-3 U/S gate.** Test builds map the identity
  range user-accessible (`#ifdef TESTING`), because `vmm_init()` runs before
  `run_tests()` and the ring-3 probe executes against the kernel map. Change one
  and change the other, or a test build triple-faults.
- **SCRUM-135 (error-code IDT vectors) resolved on `feat/x64`:** `default_stub`
  in `src/isr.s` used to do a bare `iretq` without popping the hardware error
  code that vectors 8, 10–14, 17, 21, 29, 30 push, which triple-faulted the
  machine on any page fault or GPF. A dedicated `error_stub` is now installed
  on those vectors in `idt_init()` and correctly discards the error code
  before `iretq`. See `docs/drivers/idt.md` §5/§7 for details.
- **Two allocators coexist, with different lifetimes.** `kmalloc`
  (`src/memory.c`) is the one-way bump allocator — no `free`, always 4K-aligned,
  for permanent early-boot structures (the PMM bitmap and owner table);
  `alloc_page`/`free_page` (`src/page_alloc.c`, SCRUM-7) is a bitmap PMM whose
  bitmap is itself `kmalloc`ed at init. The PMM manages **only the first usable
  region above 1 MB**, reserves the kernel/heap range and every multiboot
  module, and reports errors — double frees included — by returning quietly
  after a `serial_print`, never by faulting.
- **`malloc`/`free`/`realloc` are `kmalloc`/`kfree`/`krealloc` under libc
  names** (`src/stdlib.c`, SCRUM-30) — no second pool, no policy of their own.
  The rest of `<stdlib.h>`'s implemented subset (`atoi`, `abs`, `rand`/`srand`,
  `qsort`) lives in the same file; `calloc`, `exit`, `abort`, `atexit`,
  `getenv` and `atof` are still unimplemented. `qsort` recurses only into the
  smaller partition (the kernel stack is 16 KiB, and a both-sides recursion
  overruns it on sorted input); it partitions **three ways** (Bentley–McIlroy,
  SCRUM-171) because the two-way Hoare scan it replaced skipped over keys
  equal to the pivot and went O(n²) on any input with few distinct values —
  each scan must stop on an equal key, and the equal run that collects in the
  middle is final and belongs to neither recursion. `rand` is the C-standard
  LCG pinned to `uint32_t` — widen the accumulator and the seeded sequence
  silently changes. See `docs/syscall_spec.md` §2.2.
- **`kmalloc` is finished once `page_alloc_init` has run.** It reserves the
  bump pool as it stood at that moment and never hears about a later `kmalloc`,
  so a bump allocation made afterwards can alias a page `alloc_page()` has
  already handed out — page tables included. `kmalloc` warns on serial if you
  do it; take permanent allocations from `alloc_page()` instead, as `vmm.c`
  does.
- **COM1 serial is the only diagnostic/test output channel** right now
  (`src/serial.c`, mapped to QEMU stdio via `-serial mon:stdio`). Test framework
  output and all kernel diagnostics go through it; `serial_flush()` must be
  called before `qemu_exit()` or buffered bytes are lost.
- **Syscall entry works; six handlers are bound.** The `syscall`/`sysret`
  path is implemented (SCRUM-32): `syscall_init()` in `src/syscall.c` programs
  `EFER.SCE`/`STAR`/`LSTAR`/`FMASK`, `src/syscall_entry.s` is the entry stub,
  and `exo_syscall_dispatch` routes on the number. Bound today:
  `exo_page_alloc` (#0), `exo_page_free` (#1), `exo_page_map` (#2) and
  `exo_page_unmap` (#3) in `src/syscall_mem.c` (SCRUM-34, SCRUM-35),
  `exo_fb_acquire` (#4) in `src/syscall_fb.c` (SCRUM-154), and
  `exo_serial_write` (#8) in `src/syscall_serial.c` (SCRUM-50) — no ownership
  to check for the latter, since COM1 isn't acquired/released like the
  framebuffer; it rejects `-EXO_EFAULT` for a `[buf, buf+len)` that isn't
  entirely inside `[EXO_USER_VA_BASE, EXO_USER_VA_END)` and `-EXO_EINVAL` for
  `len` over `SERIAL_WRITE_MAX_LEN` (4096) — the whole call runs with
  interrupts off (`syscall`'s FMASK clears IF), so an uncapped write is a
  one-syscall denial of service against the timer and keyboard IRQs, not
  just slow.
  **Every other number still returns `-EXO_ENOSYS`**; binding one is
  `exo_syscall_register(EXO_SYS_*, handler)` from an `*_init()` called in
  `kernel_main` ahead of the `TESTING` branch, so the handler exists for both a
  normal boot and the test run.
  `docs/syscall_spec.md` §3 and its C expression `src/exo_syscall.h` are the
  source of truth for what each syscall must do and which doomgeneric/libc call
  sites need it; change one and change the other. The convention is
  `RAX`=number, `RDI/RSI/RDX/R10/R8/R9`=args, return in `RAX`, negative=error.
  The 4th argument is in `R10` rather than the SysV `RCX` because `syscall`
  itself overwrites `RCX` with the return RIP and `R11` with RFLAGS. The kernel
  preserves every other register, argument registers included — the stubs
  depend on it, and `tests/kernel/test_syscall_k.c` proves it from ring 3.
- **Resources are owned, and the owner is enforced.** Every managed physical
  page carries a `page_owner_t` tag (`FREE`/`KERNEL`/a LibOS id) in
  `src/page_alloc.c`, and the framebuffer has its own binding table in
  `src/fb_binding.c` — it needs one because MMIO lies outside the RAM the page
  allocator manages, so `page_owner()` cannot speak for it. `exo_page_free`
  returns `-EXO_EPERM` for a page the caller does not own (SCRUM-152),
  `exo_fb_acquire` binds the framebuffer to one context, `-EXO_EBUSY` to
  anyone else (SCRUM-154), and `exo_page_map`/`exo_page_unmap` refuse any
  `paddr` the caller neither owns nor holds the framebuffer binding for
  (SCRUM-153). **Any check on a physical page must ask `fb_binding_check_map()`
  first and only fall through to `page_owner()` when that answers
  `FB_MAP_NOT_FB`** — MMIO is invisible to the PMM, so the generic check alone
  would wave through exactly the memory the binding protects. See
  `docs/syscall_spec.md` §3.3/§3.5 and `may_map_phys()` in
  `src/syscall_mem.c`.
- **`exo_page_map` restricts the *virtual* address as well as the physical
  one.** `vaddr` must lie in `[EXO_USER_VA_BASE, EXO_USER_VA_END)` = `[64 TiB,
  128 TiB)`, else `-EXO_EPERM`. With one address space, a mapping syscall edits
  the same page tables the kernel runs on, so owning a page must not become a
  licence to install it over kernel text. **The base must stay above every
  physical address the kernel identity-maps** — the map is an identity map, so
  a window overlapping physical memory lets `exo_page_unmap` unmap kernel RAM;
  a 4 GiB base did exactly that on an 8 GiB machine. `syscall_mem_init()`
  checks it at boot. Mapping over an existing
  mapping is allowed only if the caller could have unmapped it, and that check
  refuses only a page that currently belongs to somebody else — a page owned by
  *nobody*, or framebuffer memory the caller has since lost, may still be
  dropped, because dropping a mapping changes an address space rather than a
  page. `docs/syscall_spec.md` §3.7.
- **What the kernel grants, it can take back — and the mark is an ask, not a
  seizure.** `src/revoke.c` is the revocation protocol (SCRUM-156): phase 1
  `revoke_request()` marks the resource in the ownership table, phase 2 is the
  LibOS returning it the ordinary way, phase 3 `revoke_force()`/`revoke_all()`
  takes what was not returned. A marked page **keeps its owner and stays fully
  usable** — the mark is the top bit of the `page_owner_t` tag
  (`PAGE_OWNER_REVOKED`), so **every ownership comparison in `page_alloc.c`
  must mask it off** (`owner_id()`); a raw tag compare answers `-EPERM` to the
  page's own owner and makes compliance impossible. Forced reclamation is
  scoped to the context it names — it returns `REVOKE_RETURNED`, never taking
  anything, if that context no longer holds the resource — and `revoke_all()`
  refuses `PAGE_OWNER_KERNEL`/`PAGE_OWNER_FREE` outright. `revoke_all()` is the
  hook SCRUM-155's `exo_exit` calls; nothing calls it on the boot path yet, so
  a LibOS that exits keeps its pages and the framebuffer for the rest of the
  boot. There is **no upcall to the LibOS** and no compliance deadline — phase
  1 cannot yet tell anyone it happened, which is SCRUM-147's job. Full design
  note: `docs/syscall_spec.md` §3.6.
- **The GDT in `src/boot.s` has a layout `sysret` forces, not one we chose** —
  kernel code/data at `0x08`/`0x10`, then user code32 (`0x18`, a placeholder
  long mode never loads), user data (`0x20`), user code64 (`0x28`). `sysretq`
  derives its selectors as `STAR[63:48] + 8` and `+ 16`, so reordering these or
  closing the `0x18` gap breaks every syscall return. See
  `docs/syscall_spec.md` §3.4.
- **A TSS is loaded, but `syscall` never touches it.** `src/tss.c`'s
  `tss_init()` (SCRUM-46, called from `kernel_main` right after `idt_init()`)
  installs a TSS descriptor at GDT selector `0x30` (patched into the two
  placeholder quads `boot.s` reserves there) and `ltr`s it with `RSP0`
  pointing at a dedicated 16 KiB kernel stack. This has nothing to do with the
  `syscall`/`sysretq` path — that stack swap is `syscall_entry.s`'s own doing,
  by hand, because `syscall` never consults `TSS.RSP0`. What the TSS actually
  gates is every IDT vector: `idt_set_gate` leaves `IST = 0`, so a CPL 3 → CPL
  0 exception (a page fault taken in ring 3, for instance) loads its stack
  from `TSS.RSP0` — without a TSS loaded, TR is null and that load itself
  faults, escalating through `#GP`/`#DF` to a triple fault before any handler
  runs. `tests/kernel/test_tss_k.c` drives a real ring-3 page fault to prove
  it now survives. See `docs/syscall_spec.md` §3.4/§3.7.
- **Test builds make the identity map ring-3 accessible.**
  `docker/scripts/build.sh` assembles `boot.s` with `--defsym RING3_PROBE=1`
  when `TESTING=1`, setting the U/S bit at every paging level so the ring-3
  syscall probe can run. All of physical memory is then reachable from CPL 3 —
  test builds only; a normal build keeps supervisor-only pages. SCRUM-48/-55/-56
  close this properly.
- **✅ Every libc symbol Doom needs now resolves (SCRUM-65).** `nm` over the
  compiled engine lists 52 external symbols; 32 of them had no definition
  before this. All the libc ones do now, and **`make docker-link-doom`
  (`docker/scripts/link-doom.sh`, run by CI) is the gate that keeps it that
  way** — it compiles the shim as a ring-3 LibOS target, sets it beside the
  doom objects, and fails naming any libc symbol still undefined. The only
  survivors are the four `DG_*` platform callbacks, each allowlisted in that
  script **by ticket number**; adding to that list without one is how it
  would rot into a set of exemptions.
  Three things are worth knowing before touching this area:
  - **`printf`'s missing precision was the highest-severity item in the whole
    audit**, and not because of cosmetics: `%.3d` fell to the `default:`
    branch, which echoed the text *and did not consume the vararg*, so every
    later conversion in the call read a shifted argument. `hu_stuff.c` builds
    HUD font lump names with `"STCFN%.3d"`, so Doom died in `I_Error` before
    the HUD ever drew. Precision, length modifiers, `%X`/`%o`/`%f`, `%n` and
    `*` width are all implemented now.
  - **`fopen` is a memory-mapped reader, not a filesystem.** It resolves a
    name against blobs registered via `exo_file_register_blob()` and returns a
    read-only stream over memory the kernel already mapped — 28 MB of IWAD
    that is never copied. Matching is on the **basename**, case-insensitively,
    because Doom joins a discovered directory onto a filename before calling.
    **`fopen` for writing FAILS**, as do `remove`/`rename`/`mkdir` (`EROFS`):
    a write that silently went nowhere would surface much later as a config
    that never persists or a savegame that reloads as garbage.
  - **`src/fpconv.c` exists because `-mno-sse` forbids `double` in kernel
    sources.** All of `%f` and `atof`'s real logic is integer work on an
    IEEE-754 bit pattern, so it compiles and is unit-tested from ring 0; what
    sits behind an **`__SSE2__` gate** in `stdio.c`/`stdlib.c` is two adapters
    that move 8 bytes. Measured against glibc: bit-exact for
    |decimal exponent| ≤ 15, worst case 2 ULP.
    The gate is `__SSE2__` and **not `!EXO_KERNEL`**, which is a distinction
    that has already broken a build once: `build.sh`'s `probe_cflags` is the
    kernel `CFLAGS` with only `-mcmodel` swapped, so a ring-3 link
    target inherits `-mno-sse` **unless it asks for otherwise**. "Not the
    kernel" does not imply "has SSE". As of SCRUM-66 exactly one target asks:
    `libos_doom` passes `-msse -msse2` through
    `build_ring3_link_target()`'s `extra_cflags` parameter, because Doom
    cannot compile without SSE (SCRUM-177) — and then these paths light up on their
    own.
- **The vendored Doom engine is linked, launches, and runs (SCRUM-66).**
  `src/doom/` holds doomgeneric's core (SCRUM-63). `make docker-build-doom`
  runs `docker/scripts/build-doom.sh`, which since SCRUM-64 compiles **all 79
  files with zero errors** (it was 2 of 79) and **exits non-zero if that
  regresses** — a gate, not a status report, and CI runs it.
  Since SCRUM-66 those 79 objects are also **linked into `build/exodoom`** as
  a ring-3 LibOS: `build.sh`'s `[2f/7]` step builds the `libos_doom` target
  through the same `build_ring3_link_target()` mechanism as every demo LibOS,
  and `src/syscall_launch.c` embeds the resulting blobs and launches them on
  `exo_launch(EXO_LAUNCH_APP_DOOM)` (#21, the single argument-based launch
  syscall SCRUM-184 collapsed the four per-app ones into), wired to the
  shell's `doom` command. Doom then
  completes its whole startup — `W_Init` over the mounted WAD, `R_Init`,
  `P_Init`, `S_Init`, `HU_Init`, `ST_Init`, `I_InitGraphics` — and runs its
  tick loop. What it cannot yet do is **show** anything or **read a key**:
  `DG_DrawFrame` and `DG_GetKey` are deliberate stubs in
  `src/libos_doom/libos_doom.c`, owned by SCRUM-77 and SCRUM-79.
  Three things that had to move for that link to happen, all worth knowing
  before touching the launch path:
  - `LIBOS_LAUNCH_MAX_{CODE,DATA}_PAGES` went 8/16 → **192/192**, and the
    LibOS stack went 1 page → **`LIBOS_LAUNCH_STACK_PAGES` = 16** (Doom
    recurses through the BSP tree). Doom uses ~97 and ~99 of those pages.
  - **`LIBOS_LAUNCH_STACK_TOP` is `LAYOUT_END - 8`, not `LAYOUT_END`.** The
    SysV ABI guarantees `RSP ≡ 8 (mod 16)` on entry to a called function, so
    a LibOS entered by `iretq` (which pushes no return address) must be given
    that same skew by hand or every stack frame is misaligned by 8 and the
    first aligned SSE access (`movdqa -0x40(%rbp)`) `#GP`s. Doom is the first
    ring-3 target compiled *with* SSE, so it is the first to have caught this.
  - `DG_Init` now calls `exo_file_register_blob("freedoom2.wad", ...)` after
    mounting, because `D_DoomMain` finds its IWAD through
    `D_FindIWAD`→`fopen`, not through `doom_wad_mounted()`. The blob shim
    (SCRUM-65) and the mount (SCRUM-73) existed; nothing had connected them.
  `mkdir`, `strdup` and `atof` and the `FILE*` block in `src/stdio.h` are real
  now (SCRUM-65); `docs/libc_audit.md` (SCRUM-72) remains the per-function,
  per-call-site list of what each one does and does not do — `fopen` in
  particular serves registered blobs only, and there is still no writable
  file.
  - What SCRUM-64 fixed was overwhelmingly *missing headers*, not bad vendored
    source: 77 files stopped at their first `#include`, 66 of them on
    `<strings.h>` alone (`doomtype.h` includes it on every non-Windows build).
    The doom pass now also gets `-I src` so the libc shim's headers are
    reachable at all.
  - ✅ **Doom needs SSE, and the kernel enables it (SCRUM-177).** The x86_64
    SysV ABI returns `float` in `xmm0`, so `m_config.c`'s
    `M_GetFloatVariable()` cannot compile under `-mno-sse` — `-msoft-float`
    and `-mfpmath=387` both still hit the ABI. The doom pass drops `-mno-sse`;
    the kernel build keeps it and is unaffected (no `src/*.c` uses a float).
    `src/boot.s`'s `_start64` clears CR0.EM/CR0.TS, sets CR0.MP/CR0.NE and
    CR4.OSFXSR/CR4.OSXMMEXCPT, and loads `MXCSR` = `0x1F80`, all before
    `kernel_main`; `tests/kernel/test_sse_k.c` proves SSE executes and is
    correct in ring 0 and from ring 3.
- **Nothing saves FPU/XMM state on kernel entry, and that is a decision
  (SCRUM-177), not an oversight.** `syscall_entry.s` and `isr.s` save GP
  registers only. That is safe *because* the kernel is compiled `-mno-sse
  -mno-sse2 -mno-mmx` and its hand-written assembly names no XMM register — it
  is incapable of touching a LibOS's XMM/MXCSR/x87 state, so there is nothing
  to save. `tests/kernel/sse_ring3_probe.s` / `sse_irq_probe.s` seed all 16 XMM
  registers in ring 3 and check them after a real syscall and after a real
  IRQ0 taken at CPL 3, so the invariant is CI-enforced rather than asserted.
  **Two things break it: dropping `-mno-sse` from the *kernel's* CFLAGS, and a
  second live ring-3 context** — `context_regs_t` has no XMM fields and
  `context_switch.s` no `fxsave`, so whoever makes the scheduler real owns
  adding them. Also: `CR4.OSXMMEXCPT` is set, so an unmasked SIMD exception
  would land on vector 19, where `default_stub`'s bare `iretq` loops forever;
  `MXCSR` masks all six, and anything that unmasks one owes vector 19 a
  handler first. Full reasoning: `docs/syscall_spec.md` §3.4a.
- **Trigonometry is fixed-point, and that is forced by `-mno-sse` (SCRUM-41).**
  `src/fixed_math.c/h` is the whole of what the board calls "math.h":
  `exo_fixed_sin`/`_cos`/`_tan` (Maclaurin series over a Q30 core) and
  `exo_fixed_atan` (CORDIC, vectoring mode), all in 16.16 over binary angle
  measure, plus `EXO_FIXED_FLOOR`/`_CEIL`. There is **no `double` libm and
  there cannot be one in `src/`**: the kernel compiles `-mno-sse -mno-sse2`
  and the SysV ABI returns floating point in `xmm0`, so a `double sin(double)`
  in any globbed `src/*.c` is rejected outright — the same wall
  `m_config.c`'s `M_GetFloatVariable()` hit in SCRUM-64, and the reason every
  kernel test TU could not call such an API either. Nothing under `src/doom/`
  loses by it: the engine is fixed-point throughout and its only live
  `<math.h>` call is `fabs()`.
  `tests/kernel/test_fixed_math_k.c` is the acceptance test and it is not a
  spot check — it regenerates `finesine`, `finecosine`, `finetangent` and
  `tantoangle` through this API and compares **all 24,577 entries** against
  the vendored chocolate-doom data. Where the two disagree, **this code is
  the accurate one**: `exo_fixed_sin` is correctly rounded (≤0.5 LSB from
  true), while Doom's shipped table was generated in single-precision `float`
  with a truncating cast and is off by up to 1.01 LSB — which is also why
  `finesine`'s peak is 65535 and ours is 65536. To make that comparison
  possible, `build.sh` compiles **`src/doom/tables.c`** — the one file under
  `src/doom/` the kernel image links, and only under `TESTING=1`; it is pure
  const integer arrays with zero undefined references.
- **`I_Error`/`I_Quit` report on serial and stop (SCRUM-83).** Doom's fatal
  path used to write to `stderr`, open a zenity dialog through `system()`,
  and `exit(-1)` — none of which exists here. `src/doom/i_system.c` now calls
  `src/doom_panic.c`, which is **outside** the vendored tree on purpose: it
  keeps the `i_system.c` patch small for the next re-vendor, and — since
  nothing under `src/doom/` links into `build/exodoom` yet — it is the only
  way the logic is reachable by a unit test at all.
  The API is deliberately **two** calls, not one: `doom_panic_begin()` prints
  and returns, `doom_panic_halt()` never returns. `I_Error` runs Doom's
  atexit list *between* them, so the message is already out before shutdown
  handlers run on a machine that has just declared itself broken — if one of
  them faults, it no longer takes the explanation with it. There is a
  re-entry guard, because Doom's own `already_quitting` does not actually
  stop anything (the `exit(-1)` it guards is inside `#if ORIGCODE`), and a
  recursive panic would otherwise recurse through the formatter on a ring-3
  stack that is exactly **one 4 KiB page**.
  `vprintf` was added to `src/stdio.c` for this, and **`printf` is now
  implemented in terms of it** — one formatting engine and one sink per
  build, rather than a second formatter on the panic path.
- **`DG_Init` mounts the IWAD; it does not load one (SCRUM-73).** There is no
  file to read and nowhere to put 28 MB if there were — the WAD arrives as a
  multiboot module, the kernel identity-maps it, and `libos_map_wad()` maps
  those *existing* physical pages read-only at `LIBOS_WAD_VADDR` before the
  LibOS is ever entered. So `DG_Init`'s job is to prove what's mapped really
  is a WAD, record it (`src/doom_wad.c`), and say so on serial.
  **The ticket title says "via `exo_fb_map`" and that is a mis-filing** —
  there is no such syscall (framebuffer mapping is `exo_fb_acquire` +
  `exo_page_map`, composed by `libos_fb_map()`), and it has nothing to do
  with an IWAD. The Jira description flags this on the ticket itself.
  `DG_Init` **reports rather than halts**: it records a `DOOM_WAD_*` code in
  `dg_init_result()` and returns. A missing or malformed IWAD is exactly what
  Doom's own `d_iwad.c` → `W_AddFile` path exists to diagnose, with engine
  context this layer doesn't have; halting first would pre-empt that for no
  gain. It also means there is **no `#ifdef` splitting the tested path from
  the shipped one** — `tests/kernel/test_dg_init_k.c` drives the real
  function from ring 0.
  `g_doom_params` in `src/doomgeneric_exo.c` is SCRUM-175's params hand-off,
  reusing `libos_wad_params_t` rather than declaring a second identical
  struct. It must stay the **first global in that file**, and that file first
  in the Doom target's source list, or `libos_launch_patch_params()` writes
  to the wrong offset. Its non-zero sentinel initialiser is load-bearing
  twice over: it keeps the struct out of `.bss` (where there'd be no bytes to
  patch), and it's what lets `DG_Init` report "nobody patched this" instead
  of dereferencing address 0.
- **PCM audio exists, but only in ring 0 (SCRUM-209/SCRUM-210).**
  `src/pci.c` walks the bus over config mechanism #1 and `src/hda.c` drives
  the Intel HDA controller it finds: controller reset, a CORB/RIRB command
  ring in two PMM pages, a codec widget walk that unmutes a DAC and an output
  pin, and a 64 KiB cyclic sample buffer played by the controller's own DMA
  off a two-entry BDL, with buffer completion arriving as a legacy PCI INTx
  through the existing PIC/IDT. `make docker-test`'s `hda` suite asserts the
  whole chain including `SDnLPIB` advancing and the completion IRQ firing, and
  `kernel_main` plays a 440 Hz boot tone (after `sti`, so `hda_tick()` can end
  it) as the audible half.
  **Four things will bite whoever edits this next**, all expanded in
  `docs/drivers/hda.md` §10:
  - **`SDnSTS` can only be cleared by a byte write to descriptor offset
    `0x03`.** It shares a 32-bit location with `SDnCTL`, so a dword read at
    `0x00` shows it in bits 31:24 and a dword write carrying it is silently
    dropped. `SDnSTS.BCIS` is what holds the level-triggered INTx line
    asserted, so a clear that misses re-enters the handler forever — a wedged
    boot with no fault and no output.
  - **`RINTCNT` is CORB flow control, not just an interrupt divisor.** The
    controller stops fetching commands once that many responses are unread, so
    the obvious `RINTCNT = 1` makes the command ring work exactly once unless
    `RIRBSTS` is cleared around every verb. The driver does both.
  - **The first *output* stream descriptor index is `GCAP.ISS`,** read at
    runtime — input descriptors come first in the register block, and
    hardcoding 0 programs a capture stream.
  - **`OUT_AMP_CAP`'s gain step *count* is bits 14:8, not bits 22:16 (the
    step *size*).** Getting this wrong yields a driver where every register
    reads correct, DMA runs, the IRQ fires, every test passes — and the tone
    plays ~28 dB down, i.e. inaudible. For an audio path "the registers are
    right" and "you can hear it" are different claims; settle the second with
    `-audiodev wav,id=snd0,path=out.wav` and look at the peak amplitude.
  - **`-device intel-hda` alone has no codec.** Every QEMU invocation in the
    `Makefile` and `docker/Dockerfile.qemu` now also passes
    `-device hda-output,audiodev=snd0 -audiodev none,id=snd0`; SCRUM-209 only
    needed the PCI function to exist. Audibility is `make run`'s job on a host
    with a real `-audiodev` — headless Docker has no audio backend, so CI
    asserts state up to the point samples leave RAM.
  There is no `exo_sound_pcm` and no HDA ownership binding: HDA is ring-0
  only, like `src/ata.c` was before SCRUM-188.
- **Framebuffer pixel format is BGRX8888** (empirically confirmed on QEMU),
  not RGB — relevant to anything touching `src/fb.c` or blit code.
- **The context table (SCRUM-107, `src/context.c/h`) tracks live LibOS
  contexts, but nothing switches between them yet.** `context_create()` binds
  a caller-built address space to a fresh `page_owner_t` id via
  `vmm_bind_address_space()` — the page-dir binding itself stays in `vmm.c`'s
  own per-context registry rather than being duplicated, so there's one
  source of truth for which PML4 a context runs on; `context_t` adds the
  saved-register area and scheduling state (`READY`/`RUNNING`/`BLOCKED`)
  that registry never had a field for. `CONTEXT_MAX == VMM_MAX_ADDRESS_SPACES`
  (4). The register save area is inert — `libos_enter.s`/`syscall_entry.s`
  still each carry their own single global saved-RSP slot, unchanged by this
  ticket; making that reentrant (`swapgs` + per-CPU, per
  `docs/syscall_spec.md` §3.4) and performing a real switch is SCRUM-108's
  job, and nothing here is wired into `kernel_main`'s normal boot tail.
- **`tests/kernel/kunit.h`'s `KUNIT_MAX_SUITES` had been silently exceeded on
  `main` before SCRUM-107** — 36 suites were registered against a 32-slot
  table, so `CU_add_suite` was returning `NULL` past the cap and 4 suites
  (`libos_heap_stress`, `port_io_fault`, `kernel_mem_fault`, `irq_entry`)
  were never actually running, with `ALL TESTS PASSED` still printing
  because a suite `CU_add_suite` refuses just runs zero tests, not an error
  — exactly the silent-failure mode the constant's own comment warns about.
  Raised to 48 alongside landing the context suite (the 37th) that tripped
  over it; **if `make docker-test`'s suite count looks lower than
  `test_runner.c`'s `CU_add_suite` call count, this is why — check the
  ceiling before assuming every suite ran.**
- Sprint status/roadmap and current in-flight Jira stories are tracked in
  `docs/architecture.md` §10 — check it for what's actually in progress vs.
  planned before assuming a subsystem is finished.
