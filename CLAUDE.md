# CLAUDE.md

Onboarding and quick reference for Claude Code (claude.ai/code) in this
repository. **This file is deliberately short.** Design, rationale and
per-subsystem detail live in `docs/`; this file tells you how to build, run and
test, where things are, and which doc to open next. See "Where knowledge
lives" at the bottom before adding anything here.

## Project overview

ExoDoom is a bare-metal x86_64 (long mode) **exokernel** whose goal is to run
Doom (via [doomgeneric](https://github.com/ozkl/doomgeneric)) directly on
hardware/QEMU, with no conventional OS underneath. The kernel exposes hardware
resources (physical pages, framebuffer, disk, sound, I/O) directly and pushes
policy into user-space **LibOS** programs; Doom itself runs unmodified against
a 6-function platform interface plus a libc shim.

The kernel is freestanding C (`-std=gnu99 -ffreestanding`) and x86 assembly.
No libc, no external dependencies beyond `libgcc`.

**Read `docs/architecture.md` and `docs/syscall_spec.md` before making
non-trivial changes.** They are kept up to date per sprint and hold the "why"
that isn't in the code. `docs/architecture.md` §10 says what is in progress vs.
planned.

## Build / run / test

Everything runs in Docker — no local cross-compiler is required (just Docker
and GNU Make).

```bash
make docker-build              # Build kernel + ISO -> build/exodoom, build/exodoom.iso
make docker-run                # Build, then boot the ISO in QEMU (GRUB menu)
make docker-run-kernel         # Build, then boot the kernel directly (no GRUB) -- preferred for dev iteration
make docker-test               # Build with TESTING=1, boot, stream serial test output
make docker-ci                 # Same as docker-test; what CI runs
make docker-build-doom         # Compile-only gate over vendored src/doom/ (79/79; CI runs it)
make docker-link-doom          # Link gate: every libc symbol Doom needs resolves (CI runs it)
make docker-build DEBUG=1      # Unoptimized build (-g -O0) for GDB
make docker-run-debug DEBUG=1  # Boot QEMU frozen at start, GDB stub on port 1234
make clean                     # rm -rf build
```

- QEMU shortcuts: `Ctrl+A` then `X` to exit; `Ctrl+A` then `C` for the monitor.
- Pressing Enter at the GRUB menu is currently broken — use
  `docker-run-kernel` to boot directly.
- CI (`.github/workflows/ci.yml`) runs `make docker-ci` and greps serial output
  for `ALL TESTS PASSED` / `TESTS FAILED`.
- Adding a `src/*.c` or `src/*.s` needs **no build-script change** — both are
  globbed. `src/doom/`, `src/libos_fs/` and every other *subdirectory* are
  deliberately not (they have their own build steps).
- **Every `docker-*` target leads with a `docker buildx build` of the
  cross-toolchain**, so a `wget` failure there fails all of them and says
  nothing about your tree.
- `docker/scripts/build.sh` internals (`-DEXO_KERNEL`, `-I src`, ring-3 link
  targets, toolchain mirrors and the sha256 pin): `docs/architecture.md` §9.

### Tests

Tests run **inside the kernel at boot**, not on the host — there is no way to
run one test file; `make docker-test` always runs the full suite in one QEMU
boot. To add a test:

1. Add `tests/kernel/test_<name>_k.c` using the KUnit API (`src/kunit.h`,
   CUnit-compatible). `build.sh` picks it up automatically under `TESTING=1`.
2. Register the suite in `tests/kernel/test_runner.c`'s `run_tests()` via
   `CU_add_suite(...)` + your `suite_*_tests(s)` function.
3. `make docker-test`.

**If the reported test count is lower than your `CU_add_test` count, you hit a
silent registry ceiling** (`KUNIT_MAX_SUITES` / `KUNIT_MAX_TESTS_PER_SUITE`) —
`ALL TESTS PASSED` still prints. See `docs/testing.md` "Framework limits".
Full guide: `docs/testing.md`. Debugging with GDB: `docs/debugging.md`.

## Boot flow (summary)

`GRUB` (multiboot2, requests a VESA framebuffer) → `_start` in `src/boot.s`
(builds the 4 GB identity map, enables long mode and SSE, 16 KiB stack) →
`kernel_main` in `src/kernel.c`:

```
serial_init -> idt_init -> tss_init -> mmap_init/memory_init -> page_alloc_init
  -> vmm_init -> syscall_init + every syscall_*_init
  -> pic_remap -> IRQ0 + spurious IRQ7/15 gates -> pit_init
  -> pci_init -> hda_init -> ata_init
  [-DTESTING]    serial_flush -> qemu_exit(run_tests())
  [normal boot]  fb + fbcon -> kbd_init -> sti -> launch the shell LibOS (ring 3)
```

**Anything the ring-3 tests need must be initialised before the `TESTING`
branch** — it exits QEMU and never returns. The framebuffer, keyboard and
shell launch sit below it and do **not** exist during tests. Full sequence:
`docs/architecture.md` §4.

The kernel links at `2M` (`src/linker.ld`); the boot map and kernel map are
identity maps (virtual == physical), with page 0 unmapped as a NULL guard.

## Subsystem map

| Concern | Files |
|---|---|
| Boot / entry | `src/boot.s`, `src/linker.ld`, `src/multiboot2.h`, `src/grub.cfg` |
| Memory (mmap parse, bump allocator, bitmap PMM, kernel heap) | `src/mmap.c/h`, `src/memory.c/h`, `src/page_alloc.c/h` |
| Virtual memory (kernel page tables, map/unmap/translate, per-LibOS address spaces) | `src/vmm.c/h` |
| Ring-3 LibOS launch (image build, CR3 switch, `iretq`) | `src/libos_launch.c/h`, `src/libos_enter.s`, `src/syscall_launch.c` |
| Process/context table | `src/context.c/h` |
| Interrupts (IDT/PIC/ISR, TSS, page-fault diagnostics) | `src/idt.c/h`, `src/pic.c/h`, `src/isr.s`, `src/io.h`, `src/tss.c/h`, `src/fault.c/h` |
| Timer (PIT) | `src/pit.c/h`, `src/sleep.c/h` |
| PCI bus | `src/pci.c/h` |
| Intel HDA audio | `src/hda.c/h` |
| PC speaker | `src/speaker.c/h` |
| Doom sound (tone table, speaker sequencer, PCM path) | `src/doom_sfx_tone.c/h`, `src/doom_sound.c/h`, `src/doom_sound_pcm.c/h` |
| WAD DMX sound lump decoder | `src/doom_dmx.c/h` |
| MUS/MIDI song reader + sequencer (ring 3 only) | `src/libos_doom/doom_midi.c/h` |
| GENMIDI OPL instrument decoder (ring 3 only) | `src/libos_doom/doom_genmidi.c/h` |
| OPL3 synth + MIDI→OPL voice allocator (ring 3 only) | `src/libos_doom/doom_opl.c/h`, vendored `src/nuked_opl3/` |
| Doom `music_module_t` logic (register/play/poll/pause over the OPL3 synth and the stream; ring 3, TESTING-kernel suite) | `src/libos_doom/doom_sound_music.c/h` (glue table in `src/doom_sound.c`) |
| Software PCM mixer (8 sfx voices + 1 music voice) | `src/pcm_mixer.c/h` (+ `hda_pcm_*` in `src/hda.c/h`) |
| Serial (COM1, all diagnostic + test output) | `src/serial.c/h` |
| Framebuffer + text console | `src/fb.c/h`, `src/fb_console.c/h` |
| Syscall gate (entry, dispatch, handlers) | `src/syscall.c/h`, `src/syscall_entry.s`, `src/syscall_*.c/h` (one per area) |
| Resource ownership (secure binding) | `src/page_alloc.c/h` (pages), `src/fb_binding.c/h`, `src/disk_binding.c/h` |
| Resource revocation | `src/revoke.c/h` and the `*_revoke_*` primitives |
| Disk (ATA PIO) | `src/ata.c/h`, `src/syscall_disk.c/h` |
| LibOS-space filesystem (ExoFS) | `src/libos_fs/` — **not part of the kernel** |
| Keyboard / mouse | `src/ps2.c/h`, `src/kbd_ring.c/h`, `src/ps2_mouse.c/h` |
| Freestanding libc | `src/string.c/h`, `src/ctype.c/h`, `src/stdio.c/h`, `src/stdlib.c/h`, `src/errno.c/h`, `src/fpconv.c/h`; header-only shims `src/strings.h`, `src/inttypes.h`, `src/math.h`, `src/unistd.h`, `src/assert.h`, `src/fcntl.h`, `src/sys/*.h` |
| Fixed-point trigonometry | `src/fixed_math.c/h` |
| Vendored Doom engine (`libos_doom` ring-3 target) | `src/doom/`, `src/libos_doom/` |
| doomgeneric platform layer | `src/doomgeneric_exo.c/h` |
| Doom net stub / fatal-error path / mounted IWAD registry | `src/doom_net_stub.c`, `src/doom_panic.c/h`, `src/doom_wad.c/h`, `src/doom_wad_disk.c/h` |
| LibOS apps | `src/shell/`, `src/libos_wad_viewer/`, `src/libos_clock/`, … |
| Test framework | `src/kunit.h`, `tests/kernel/*.c`, `tests/kernel/kunit.c`, `tests/kernel/ring3_probe.s` |

## Gotchas that bite first

One line each; the linked doc has the reasoning. Read it before editing the
area.

**Build and tests**
- Ring-3 test builds map the identity range user-accessible
  (`--defsym RING3_PROBE=1`), and `src/vmm.c` mirrors that under `#ifdef
  TESTING` — change one and change the other, or a test build triple-faults.
  → `docs/architecture.md` §9, `docs/memory.md` §7.
- Compiled ring-3 targets are linked at the real LibOS addresses and need
  `.text.entry`, a first-global params struct and the entry TU linked first.
  → `docs/architecture.md` §7 "LibOS launch conventions".
- Never mix `__SSE2__` and `EXO_KERNEL` as the same condition: "not the kernel"
  does not mean "has SSE". → `docs/architecture.md` §7 "Linking the engine".
- Kernel code is `-mno-sse` and kernel entry saves no XMM state, by design.
  Dropping that flag breaks a CI-enforced invariant. → `docs/syscall_spec.md`
  §3.4a. Consequence: no `double` anywhere in `src/` (`src/fixed_math.c`
  instead → §2.4).
- COM1 serial is the only diagnostic/test channel; `serial_flush()` before
  `qemu_exit()` or output is lost. → `docs/drivers/serial.md`.

**Memory and ownership**
- `kmalloc`/`malloc` are the kernel heap; page tables come from `alloc_page()`.
  → `docs/memory.md` §5, §6b.
- Any check on a physical page asks `fb_binding_check_map()` first, then
  `page_owner()`; every compare in `page_alloc.c` masks the revoke bit
  (`owner_id()`). → `docs/syscall_spec.md` §3.3, §3.6.
- `exo_page_map` restricts the *virtual* address to `[EXO_USER_VA_BASE,
  EXO_USER_VA_END)`, which must stay above all identity-mapped RAM.
  → `docs/syscall_spec.md` §3.7.
- The GDT layout in `boot.s` is forced by `sysret` — do not reorder it.
  → `docs/syscall_spec.md` §3.4.

**Interrupts and drivers**
- Vectors `0x27`/`0x2F` have real spurious-IRQ handlers; anything claiming
  them must make the same ISR check. → `docs/drivers/pic.md` §9.
- HDA: `SDnSTS` is cleared by a byte write at `0x03`; `RINTCNT` is flow
  control; the first output stream index is `GCAP.ISS`. → `docs/drivers/hda.md`
  §10.
- Mixer and stream design, including why `src/pcm_mixer.c` includes no device
  header. → `docs/drivers/hda.md` §12.

**Filesystem and Doom**
- ExoFS is LibOS code over `exo_disk_*`; disk buffers come from
  `libos_heap_alloc()`, never `malloc()`; unmount before releasing the disk;
  write ordering is load-bearing. → `docs/filesystem.md` §1, §8, "Pitfalls".
- `fopen` is a read-only blob reader; writes fail (`EROFS`) on purpose.
  → `docs/architecture.md` §7, `docs/libc_audit.md`.
- Doom's net layer is two booleans; flipping `drone` silently freezes the
  game; `FEATURE_MULTIPLAYER` must stay undefined. → `docs/architecture.md` §7
  "Networking".
- `DG_Init` mounts the IWAD, it does not load it, and reports instead of
  halting. → `docs/architecture.md` §7.

## Where knowledge lives

| Topic | Doc |
|---|---|
| System design, boot sequence, subsystems, LibOS/Doom path, build, sprint status | `docs/architecture.md` |
| Syscall ABI, ownership/revocation, address-space mapping, libc audit, sound | `docs/syscall_spec.md` |
| Paging, allocators, LibOS heap | `docs/memory.md` |
| ExoFS | `docs/filesystem.md` |
| KUnit, writing and registering tests | `docs/testing.md` |
| GDB and bare-metal debugging | `docs/debugging.md` |
| Branches, commits, style, CI checks | `docs/contributing.md` |
| libc per-function audit | `docs/libc_audit.md` |
| Drivers (IDT, PIC, PIT, serial, keyboard, framebuffer, ATA, PCI, HDA, speaker) | `docs/drivers/*.md` |

**New architectural facts go into the doc that owns the subsystem, integrated
into its existing section — not appended here.** If you find yourself writing
a multi-line, ticket-numbered paragraph in this file, it belongs in `docs/`;
leave at most a one-line pointer.
