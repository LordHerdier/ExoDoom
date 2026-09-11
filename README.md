# Exokernel Doom development repo

ExoDoom is a bare-metal x86_64 (long mode) exokernel: the goal is to run Doom
(via [doomgeneric](https://github.com/ozkl/doomgeneric)) directly on
hardware/QEMU with no conventional OS underneath. See `docs/architecture.md`
for the full design and `docs/syscall_spec.md` for the syscall/libc audit.

## Repo layout

- `src/`
  - `boot.s` — Multiboot2 header + `_start` entry: builds a static 4 GB
    identity map, enters long mode, sets up the initial stack, calls
    `kernel_main`
  - `kernel.c` — freestanding C kernel entrypoint; brings up memory, paging,
    the syscall gate, interrupts, and the framebuffer console
  - `linker.ld` — links the kernel at 2M, defines sections, entrypoint `_start`
  - `grub.cfg` — GRUB menu entry that multiboot2-loads `/boot/exodoom` and the
    `freedoom2.wad` module
  - `vmm.c/h`, `memory.c/h`, `mmap.c/h`, `page_alloc.c/h` — paging and physical
    memory management
  - `idt.c/h`, `pic.c/h`, `isr.s`, `tss.c/h`, `fault.c/h` — interrupts,
    exceptions, and the page-fault handler
  - `syscall.c/h`, `syscall_entry.s`, `syscall_mem.c/h`, `syscall_fb.c/h` —
    the `syscall`/`sysret` gate and its handlers
  - `fb.c/h`, `fb_console.c/h`, `fb_binding.c/h` — framebuffer output and
    ownership
  - `serial.c/h` — COM1 serial, the only diagnostic/test output channel
  - `doom/` — vendored [doomgeneric](https://github.com/ozkl/doomgeneric) core
    engine source (GPL-2.0); not yet built into the kernel (see below)
- `tests/kernel/` — KUnit-style kernel tests, run in-kernel at boot under
  `TESTING=1` (see `docs/testing.md`)
- `docker/`
  - `Dockerfile.build` — builds an `x86_64-elf` cross toolchain (binutils +
    GCC) plus GRUB/ISO tooling
  - `Dockerfile.qemu` — QEMU runtime image
  - `scripts/build.sh` — build script run inside the build container
  - `scripts/build-doom.sh` — best-effort compile pass over `src/doom/`
- `Makefile` — convenience targets to build, run, and test

## Dependencies

### Required (host machine)
- Docker (or compatible: Podman should work if it supports `docker` CLI well enough)
- GNU Make

That's it. No local cross-compiler required.

### What Docker images provide
- An `x86_64-elf` cross compiler (`x86_64-elf-gcc` / `x86_64-elf-as`), built
  from source (binutils + GCC) in `Dockerfile.build`
- GRUB + ISO tooling inside the build container (`grub-mkrescue`, `grub-file`,
  `xorriso`)
- QEMU inside the run container (`qemu-system-x86_64`)

## How the build works (high level)

The build container runs `docker/scripts/build.sh`:

1. Fetch the Freedoom v0.13.0 `freedoom2.wad` IWAD, verified against a
   pinned sha1 and cached at `build/freedoom2.wad` (re-downloaded only if
   missing or the hash doesn't match — see "Assets fetched at build time"
   below)
2. Assemble `src/boot.s` → `build/boot.o`
3. Verify the identity map's U/S protection matches the build type (supervisor-only
   for a normal build, user-accessible for a `TESTING=1` build so the ring-3
   syscall probe can run — see `docs/memory.md` §7)
4. Compile every `src/*.c` (freestanding, `-DEXO_KERNEL`) and assemble every
   other `src/*.s`; under `TESTING=1`, also compile `tests/kernel/*.c`
   and assemble `tests/kernel/*.s`
5. Link everything with `src/linker.ld` (`-nostdlib -lgcc`) → `build/exodoom`
6. Verify it's Multiboot2-valid using `grub-file`
7. Stage an ISO tree under `build/isodir/boot/...` and create
   `build/exodoom.iso` using `grub-mkrescue`

Outputs:
- `build/exodoom` (kernel ELF)
- `build/exodoom.iso` (bootable ISO, including `freedoom2.wad` as a GRUB
  Multiboot2 module)

## Makefile usage

### Build everything (kernel + ISO) in Docker
```bash
make docker-build
```

### Build, then boot the ISO in QEMU (GRUB menu)

```bash
make docker-run
```

### Build, then boot the kernel directly (no GRUB menu) <--- Recommended for development iteration

```bash
make docker-run-kernel
```

### Build with `TESTING=1`, boot, and stream serial test output

```bash
make docker-test
```

This runs the in-kernel KUnit test suite (`tests/kernel/`) and prints the
serial log, ending in `ALL TESTS PASSED` or `TESTS FAILED`. See
`docs/testing.md` for how to add a test.

### Same as `docker-test`; what CI runs

```bash
make docker-ci
```

### Unoptimized build for GDB (`-g -O0`), and a frozen-boot debug target

```bash
make docker-build DEBUG=1
make docker-run-debug DEBUG=1   # boots QEMU halted, GDB stub on port 1234
```

### Best-effort compile pass over vendored `src/doom/` (not part of CI)

```bash
make docker-build-doom
```

### Clean build artifacts

```bash
make clean
```

## Keyboard shortcuts
- `Ctrl + A` then `X` to exit QEMU
- `Ctrl + A` then `C` to open QEMU monitor (for debugging)

## Notes / gotchas

* Pressing Enter at the GRUB menu doesn't work right now. Boot directly for the time being, unless debugging the GRUB config.
* QEMU is run with `-display curses` and serial attached to your terminal (`-serial mon:stdio`).
* The kernel drives a linear framebuffer console (BGRX8888) rather than VGA
  text mode — GRUB is asked for a VESA framebuffer at boot, and `fb_console.c`
  renders text into it.
* If you edit `src/grub.cfg`, it gets copied into the ISO staging directory during build.

## Third-party code
[doomgeneric](https://github.com/ozkl/doomgeneric) is vendored under `src/doom/`
as the Doom engine this kernel targets — see `src/doom/LICENSE` (GPL-2.0). It
is not yet linked into `build/exodoom`.

## Assets fetched at build time
`freedoom2.wad` isn't committed to the repo — `docker/scripts/build.sh` fetches
it from the official [Freedoom](https://github.com/freedoom/freedoom) v0.13.0
release on every build, pinned and verified by sha1/sha256 rather than trusted
blindly, and fails the build loudly on a hash mismatch. It's cached at
`build/freedoom2.wad` so a rebuild doesn't re-download it. See
`docs/architecture.md` §7 for the exact pin and how it was verified.

## License
MIT License (at least for now, may change later...?)
