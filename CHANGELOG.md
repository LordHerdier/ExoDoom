# Changelog

## 2026-09-14 to 2026-09-15

Sixteen PRs landed on `main` in this window (#65–#80). The doom port gate
went from "2/79 compiles" to a full compile pass with float/SSE support, a
real process/context table and context switch landed alongside the first
two scheduling-adjacent syscalls (`exo_yield`, `exo_exit` resource
reclamation), and the boot demo grew a minimal terminal/shell LibOS that
launches the WAD/flat/automap viewer interactively instead of it being a
fixed boot-time sequence.

### Added

- **Process/context table** (SCRUM-107, #73) — `src/context.c/h`:
  `context_create()` binds a caller-built address space to a fresh
  `page_owner_t` id via `vmm_bind_address_space()`, and adds the saved-register
  area and scheduling state (`READY`/`RUNNING`/`BLOCKED`) the VMM's own
  registry never had a field for. `CONTEXT_MAX == VMM_MAX_ADDRESS_SPACES` (4).
- **Real context switch** (SCRUM-108, #75) — `src/context_switch.s`: saves and
  restores general-purpose registers and swaps CR3 between two live contexts,
  built on top of the context table above.
- **`exo_yield` syscall** (SCRUM-109, #76) — cooperative switching between
  contexts through the context-switch primitive.
- **LibOS resource reclamation on exit** (SCRUM-155, #77) — `src/syscall_exit.c/h`:
  `exo_exit` now calls `revoke_all()` so a context's pages (and framebuffer
  binding, if held) are reclaimed instead of leaking for the rest of boot.
- **Minimal terminal/shell LibOS** (SCRUM-110, #78) — replaces the fixed
  boot-time demo sequence in `kernel_main` with an interactive shell LibOS
  that launches other demos on request.
- **WAD/flat/automap viewer forward-ported into the shell** (SCRUM-178, #80) —
  `src/automap.c/h` plus companion flat/WAD viewer code, now launched as a
  shell command instead of running unconditionally at boot.
- **SSE enabled in the kernel** (SCRUM-177, #79) — `src/boot.s`'s `_start64`
  clears CR0.EM/CR0.TS, sets CR0.MP/CR0.NE and CR4.OSFXSR/CR4.OSXMMEXCPT, and
  loads `MXCSR = 0x1F80` before `kernel_main`, which is what Doom's
  `M_GetFloatVariable()` needs (`float` returns in `xmm0` under the SysV
  ABI) — the doom pass also drops `-mno-sse`, while the kernel build keeps it
  since no kernel `.c` file touches a float. Proven from ring 0 and ring 3 by
  `tests/kernel/test_sse_k.c`.
- **All 79 vendored `src/doom/` files now compile** (SCRUM-64, #66), up from
  2/79 — overwhelmingly missing headers (`<strings.h>` alone stopped 66 of
  them), not bad vendored source; `docker/scripts/build-doom.sh` now exits
  non-zero on any regression and CI runs it as a gate.
- **libc audit of every function Doom calls** (SCRUM-72, #67) —
  `docs/libc_audit.md`/`.csv` plus `docs/gen_libc_audit.py`: per-function,
  per-call-site table of what's real, stubbed, or still owed.
- **`DG_GetTicksMs` / `DG_SleepMs`** (SCRUM-74, #68) — `src/doomgeneric_exo.c`:
  the timer half of the doomgeneric platform layer, built on `exo_get_ticks`.
- **`exo_kbd_poll` (#6) bound** (SCRUM-39, #69) — `src/syscall_kbd.c/h`: wires
  the keyboard event ring into the syscall gate.
- **Hardware interrupts at CPL 3 verified to switch to `TSS.RSP0`**
  (SCRUM-170, #70) — `tests/kernel/irq_entry_probe.s`: a real IRQ0 taken while
  running ring-3 LibOS code is proven to land on the kernel stack, not the
  interrupted ring-3 one.
- **`exo_errno.h`** (SCRUM-57, #71) — syscall error codes extracted out of
  `exo_syscall.h` into their own header, documented in `docs/syscall_spec.md`.

### Fixed

- **Timer demo drift** (SCRUM-163, #72) — `src/sleep.c/h`: corrected
  accumulation error in the sleep/timing demo path.
- **`fb_console` scroll now moves whole 32-bit pixels, not bytes**
  (SCRUM-162, #74) — off-by-shift in the scroll routine that could tear or
  misalign rows; new `test_fb_console_k.c` suite covers it.
- **SCRUM-16 + SCRUM-36 formally landed on `main`** (#65) — the framebuffer/WAD
  mapping accessor and LibOS-side `libos_fb_map()` helper documented in the
  prior entry merged to `main` in this window.

### Documentation

- `docs/architecture.md` and `docs/syscall_spec.md` updated for the process/
  context table, context switch, `exo_yield`, resource reclamation, the
  terminal/shell LibOS, and SSE enablement.
- `CLAUDE.md` kept current with the doom-compile gate, SSE rationale, and the
  new context/scheduling subsystem.

### Contributors

Charlie Whittleman, Bryan Simpson, Charlotte, carlitodavis.

## 2026-09-08 to 2026-09-14

Covers the past week of work on the `demo`/`main` branches: the virtual memory
manager and per-LibOS address spaces landed, the syscall gate grew from four
bound handlers to eight, ring-3 LibOS launch went from "boots to ring 3" to a
real entry convention with a ported libc shim, resource ownership/revocation is
now enforced end-to-end (pages, framebuffer, kernel memory, port I/O), and the
boot demo now renders WAD flats and an interactive automap.

### Added

- **Per-LibOS address spaces & virtual memory manager** (SCRUM-15, SCRUM-48) —
  `src/vmm.c/h`: builds the kernel's real page tables from PMM pages,
  identity-maps low memory/kernel image/usable RAM/multiboot info/framebuffer,
  and adds `vmm_create_address_space`/`vmm_destroy_address_space`/
  `vmm_switch_address_space` plus a `page_owner_t`-keyed registry
  (`vmm_bind_address_space`/`vmm_address_space_for`) so each LibOS can get its
  own PML4 sharing the kernel's PML4[0].
- **`exo_page_map` / `exo_page_unmap` syscalls** (SCRUM-35) — map/unmap
  arbitrary owned physical pages into a caller-chosen virtual address,
  restricted to the `[EXO_USER_VA_BASE, EXO_USER_VA_END)` window, which was
  moved above all identity-mapped physical memory (#39) so a mapping syscall can
  never be used to unmap kernel RAM.
- **Real ring-3 LibOS launch** (SCRUM-47, SCRUM-49, SCRUM-50) —
  `src/libos_launch.c/h`
  - `src/libos_enter.s`: builds a fresh address space, copies code/data/bss to
    fixed virtual addresses
    (`LIBOS_LAUNCH_CODE_VADDR`/`_DATA_VADDR`/`_STACK_VADDR`), and `iretq`s to
    CPL 3. Defines and proves the `libos_main()` PIC entry convention, and binds
    `exo_serial_write` (#8) as the first syscall callable from real ring-3 code.
- **Separate LibOS link target for compiled ring-3 code** (SCRUM-173) — new
  `tests/kernel/ring3_link_target.ld.in` template lets ordinary compiled C run
  at the real LibOS launch addresses instead of hand-assembled PIC probes.
- **libc shim ported to real ring-3 syscalls** (SCRUM-51) — `stdlib.c`/`stdio.c`
  compiled a second time without `-DEXO_KERNEL`, routing
  `malloc`/`free`/`realloc` through `libos_heap`/`libos_page_alloc` and `printf`
  through `exo_serial_write`, proven end-to-end by the new `libc_shim_probe`
  link target.
- **LibOS-side page allocator and heap allocator** (SCRUM-37, SCRUM-38) —
  `src/libos_page_alloc.c/h`, `src/libos_heap.c/h`: user-space allocators built
  on top of `exo_page_alloc`/`exo_page_map`, usable both in-process (kernel-side
  tests) and via the real syscall stubs (ring-3 builds).
- **LibOS-side framebuffer mapping** (SCRUM-36) — `src/libos_fb.c/h`: composes
  `exo_fb_acquire` + `exo_page_map` since there is no dedicated `exo_fb_map`
  syscall; proven by writing/reading known pixels through the mapped range.
- **Named WAD-module accessor** (SCRUM-16) —
  `mmap_find_module()`/`mmap_get_info()` in `src/mmap.c/h` as the single place
  that locates the WAD module, plus a `vmm_fb_wad` test suite proving both the
  WAD and framebuffer mappings survive paging being enabled.
- **Page fault handler** (SCRUM-17) — `src/fault.c/h`: trap-14 handler with
  serial diagnostics (faulting address, error code, present/write/user bits).
- **TSS for ring-3 exceptions** (SCRUM-46) — `src/tss.c/h`: installs a TSS
  descriptor and loads `RSP0` so a CPL 3 → CPL 0 exception (e.g. a ring-3 page
  fault) doesn't triple-fault.
- **Per-page ownership tracking** (SCRUM-152) — `page_owner_t` tag on every
  managed physical page (`FREE`/`KERNEL`/LibOS id); `exo_page_free` now rejects
  frees of pages the caller doesn't own.
- **`exo_page_alloc` / `exo_page_free` syscalls** (SCRUM-34).
- **Framebuffer secure binding** (SCRUM-154) — `src/fb_binding.c/h`:
  `exo_fb_acquire` binds the framebuffer to one context, `-EXO_EBUSY` to anyone
  else, since MMIO falls outside PMM-managed RAM and needs its own ownership
  table.
- **Resource revocation / repossession protocol** (SCRUM-156) —
  `src/revoke.c/h`: three-phase mark/return/force-reclaim protocol for pages and
  the framebuffer binding.
- **`exo_fb_acquire` bounds-checking** (SCRUM-54) — `info_out` is now checked
  against the LibOS window before being written to.
- **`exo_get_ticks` bound to a PIT-backed handler** (SCRUM-172) —
  `src/syscall_pit.c/h`; PIT/PIC init moved ahead of the `TESTING` branch so
  ring-3 tests can use it.
- **Ring-3 protection tests**: LibOS cannot read/write kernel memory — GPF on
  access (SCRUM-55); LibOS cannot execute `IN`/`OUT` — GPF on port I/O
  (SCRUM-56), backed by a new fault-diagnostic path in `src/fault.c`.
- **First-fit kernel heap** (SCRUM-25) — `src/heap.c/h`, with a 10K-block stress
  test and leak audit (SCRUM-27).
- **`<stdlib.h>` wrappers** (SCRUM-30) — `malloc`/`free`/`realloc` as
  `kmalloc`/`kfree`/`krealloc`, plus `atoi`, `abs`, `rand`/`srand`, `qsort`.
- **Three-way qsort partition** (SCRUM-171) — Bentley–McIlroy three-way
  partitioning, fixing O(n²) behavior on inputs with many equal keys; recurses
  only into the smaller side to stay within the 16 KiB kernel stack.
- **`strcasecmp` / `strncasecmp`** (SCRUM-43).
- **Pinned Freedoom2 IWAD** (SCRUM-164) — vendored and pinned so the build
  doesn't depend on a network fetch at build time.
- **WAD/automap boot demo** — forward-ported `wad.c`/`flat.c`/`automap.c` from
  an older demo branch: renders every flat texture in `freedoom2.wad` as a grid,
  then an interactive `<-`/`->` automap viewer over each `MAPxx` lump driven by
  the real `exo_kbd_poll` syscall path.
- **CI: cache the cross-compiler builder image** (#48) — speeds up `docker-ci`
  runs.

### Fixed

- **`page_alloc_init()` bump allocator clobbering GRUB module memory** — GRUB
  places a module (the WAD) immediately after the kernel image, the same address
  the bump allocator starts handing out from; nothing previously read module
  content at runtime so this was never hit. The bump pointer now skips past
  every module, and the multiboot info struct itself is also reserved
  (identity-mapping it was not the same as reserving it).

### Documentation

- `docs/syscall_spec.md`, `docs/architecture.md`, `docs/memory.md`, and
  `docs/drivers/idt.md` updated throughout the week to track each syscall,
  ownership rule, and memory-layout change above.
- README updated (SCRUM-167) and CLAUDE.md kept current with each new subsystem
  and build-script mechanism.

### Contributors

Charlie Whittleman, Bryan Simpson.
