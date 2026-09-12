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
make docker-build-doom         # Best-effort compile pass over vendored src/doom/ (NOT part of CI)
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
  -> mmap_init(mb) -> memory_init() -> page_alloc_init(mb) -> vmm_init(mb, fb)
  -> syscall_init() -> syscall_mem_init() -> syscall_fb_init(fb)
  [if -DTESTING]  serial_flush() -> qemu_exit(run_tests())
  [normal boot]   fb_init_bgrx8888() + fbcon_init()  (halts if absent)
                  -> banner/mmap dump -> ownership self-check -> idt_init()
                  -> pic_remap() -> idt_set_gate(32, irq0_stub) -> pit_init(1000)
                  -> idt_set_gate(33, irq1_stub) -> kbd_init()
                  -> sti -> `sti; hlt` idle loop
```

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
than one undifferentiated blob (SCRUM-49) — see below. v1's one LibOS still
runs on the kernel's own map on a normal boot, because nothing yet defines
what runs after the jump (SCRUM-50's `libos_main()`).

### Subsystem map

| Concern | Files |
|---|---|
| Boot / entry | `src/boot.s`, `src/linker.ld`, `src/multiboot2.h`, `src/grub.cfg` |
| Memory (mmap parse, bump allocator, bitmap PMM) | `src/mmap.c/h`, `src/memory.c/h`, `src/page_alloc.c/h` |
| Virtual memory (kernel page tables, map/unmap/translate) | `src/vmm.c/h` |
| Ring-3 LibOS launch (image build, CR3 switch, `iretq`) | `src/libos_launch.c/h`, `src/libos_enter.s` |
| Interrupts (IDT/PIC/ISR, TSS, page-fault diagnostics) | `src/idt.c/h`, `src/pic.c/h`, `src/isr.s`, `src/io.h`, `src/tss.c/h`, `src/fault.c/h` |
| Timer (PIT) | `src/pit.c/h`, `src/sleep.c/h` |
| Serial (COM1, all diagnostic + test output) | `src/serial.c/h` |
| Framebuffer + text console | `src/fb.c/h`, `src/fb_console.c/h` |
| Syscall gate (entry, dispatch, handlers) | `src/syscall.c/h`, `src/syscall_entry.s`, `src/syscall_mem.c/h`, `src/syscall_fb.c/h` |
| Resource ownership (secure binding) | `src/page_alloc.c/h` (pages), `src/fb_binding.c/h` (framebuffer) |
| Resource revocation (repossession) | `src/revoke.c/h` (protocol), the `page_revoke_*`/`fb_binding_revoke_*` primitives |
| Keyboard (PS/2 + event ring) | `src/ps2.c/h`, `src/kbd_ring.c/h` |
| Freestanding libc bits | `src/string.c/h`, `src/ctype.c/h`, `src/stdio.c/h`, `src/stdlib.c/h` |
| Vendored Doom engine (not yet linked) | `src/doom/` |
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
  in `src/libos_launch.h`, each spanning up to
  `LIBOS_LAUNCH_MAX_{CODE,DATA}_PAGES` pages rather than the single
  undifferentiated page SCRUM-47 shipped with — and `libos_enter()` switches
  CR3 and `iretq`s to CPL 3 — proven by `tests/kernel/test_libos_launch_k.c`
  against a real, separate address space rather than the TESTING-only
  blanket-user-accessible trick `tests/kernel/ring3_probe.s` uses. This is
  still a fixed-size loader, not a general one: `code`/`data` are copied
  verbatim into position, position-independent by convention (no ELF, no
  relocation), and oversized input is rejected as `VMM_EINVAL` before
  anything is allocated. What SCRUM-49 does *not* do is wire this into
  `kernel_main` or say what runs once execution reaches
  `LIBOS_LAUNCH_CODE_VADDR`: on a normal boot, `kernel_main` still binds the
  one v1 LibOS to `vmm_kernel_pml4()` itself, a placeholder that keeps it
  running on the kernel's own map, because SCRUM-50 hasn't yet defined
  `libos_main()`'s calling convention and the normal boot tail is a live
  interactive demo, not a placeholder waiting to be replaced. Read
  `docs/memory.md` §7, `docs/syscall_spec.md` §3.7 and `docs/architecture.md`
  §5.1/§6 before implementing anything in that space.
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
  overruns it on sorted input), and `rand` is the C-standard LCG pinned to
  `uint32_t` — widen the accumulator and the seeded sequence silently changes.
  See `docs/syscall_spec.md` §2.2.
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
- **Syscall entry works; five handlers are bound.** The `syscall`/`sysret`
  path is implemented (SCRUM-32): `syscall_init()` in `src/syscall.c` programs
  `EFER.SCE`/`STAR`/`LSTAR`/`FMASK`, `src/syscall_entry.s` is the entry stub,
  and `exo_syscall_dispatch` routes on the number. Bound today:
  `exo_page_alloc` (#0), `exo_page_free` (#1), `exo_page_map` (#2) and
  `exo_page_unmap` (#3) in `src/syscall_mem.c` (SCRUM-34, SCRUM-35), and
  `exo_fb_acquire` (#4) in `src/syscall_fb.c` (SCRUM-154).
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
- **The vendored Doom engine is present but not built.** `src/doom/` holds
  doomgeneric's core (SCRUM-63) and is excluded from the normal build on
  purpose — many files still need libc gaps filled. `make docker-build-doom`
  runs `docker/scripts/build-doom.sh` as a best-effort compile pass; it is not
  part of `docker-build` or CI, and nothing in `src/doom/` links into
  `build/exodoom` yet.
- **Framebuffer pixel format is BGRX8888** (empirically confirmed on QEMU),
  not RGB — relevant to anything touching `src/fb.c` or blit code.
- Sprint status/roadmap and current in-flight Jira stories are tracked in
  `docs/architecture.md` §10 — check it for what's actually in progress vs.
  planned before assuming a subsystem is finished.
