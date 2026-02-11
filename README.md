# Exokernel Doom development repo

## Repo layout

- `src/`
  - `boot.s` — Multiboot header + `_start` entry (sets up stack, calls `kernel_main`)
  - `kernel.c` — freestanding C kernel entrypoint (writes to VGA text buffer)
  - `linker.ld` — places kernel at 2M, defines sections, entrypoint `_start`
  - `grub.cfg` — GRUB menu entry that multiboot-loads `/boot/exodoom`
- `docker/`
  - `Dockerfile.build` — i686-elf cross compiler + GRUB ISO tools
  - `Dockerfile.qemu` — QEMU runtime image
  - `scripts/build.sh` — build script run inside the build container
- `Makefile` — convenience targets to build + run

## Dependencies

### Required (host machine)
- Docker (or compatible: Podman should work if it supports `docker` CLI well enough)
- GNU Make

That’s it. No local cross-compiler required.

### What Docker images provide
- `techiekeith/gcc-cross-i686-elf` base image for `i686-elf-gcc` / `i686-elf-as`
- GRUB + ISO tooling inside the build container (`grub-mkrescue`, `grub-file`, `xorriso`)
- QEMU inside the run container (`qemu-system-i386`)

## How the build works (high level)

The build container runs `docker/scripts/build.sh`:

1. Assemble `src/boot.s` → `build/boot.o`
2. Compile `src/kernel.c` (freestanding) → `build/kernel.o`
3. Link with `src/linker.ld` → `build/exodoom`
4. Verify it’s Multiboot-valid using `grub-file`
5. Stage an ISO tree under `build/isodir/boot/...`
6. Create `build/exodoom.iso` using `grub-mkrescue`

Outputs:
- `build/exodoom` (kernel ELF)
- `build/exodoom.iso` (bootable ISO)

## Makefile usage

### Build everything (kernel + ISO) in Docker
```bash
make docker-build
````

### Build, then boot the ISO in QEMU (GRUB menu)

```bash
make docker-run
```

### Build, then boot the kernel directly (no GRUB menu) <--- Recommended for development iteration

```bash
make docker-run-kernel
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
* The kernel currently writes to VGA text memory at `0xB8000`, so you should see output in the QEMU display.
* If you edit `src/grub.cfg`, it gets copied into the ISO staging directory during build.

## License
MIT License (at least for now, may change later...?)