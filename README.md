# Exokernel Doom development repo

## Repo layout

- `src/`
  - `boot.s` — Multiboot header + `_start` entry (sets up stack, calls `kernel_main`)
  - `kernel.c` — freestanding C kernel entrypoint (writes to VGA text buffer)
  - `linker.ld` — places kernel at 2M, defines sections, entrypoint `_start`
  - `grub.cfg` — GRUB menu entry that multiboot-loads `/boot/exodoom`
  - `doom/` — vendored [doomgeneric](https://github.com/ozkl/doomgeneric) core engine source (GPL-2.0), used as the Doom port target for this exokernel
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

1. Fetch the Freedoom v0.13.0 `freedoom2.wad` IWAD, verified against a
   pinned sha1 and cached at `build/freedoom2.wad` (re-downloaded only if
   missing or the hash doesn't match — see "Assets fetched at build time"
   below)
2. Assemble `src/boot.s` → `build/boot.o`
3. Compile `src/kernel.c` (freestanding) → `build/kernel.o`
4. Link with `src/linker.ld` → `build/exodoom`
5. Verify it’s Multiboot-valid using `grub-file`
6. Stage an ISO tree under `build/isodir/boot/...`
7. Create `build/exodoom.iso` using `grub-mkrescue`

Outputs:
- `build/exodoom` (kernel ELF)
- `build/exodoom.iso` (bootable ISO, including `freedoom2.wad` as a GRUB
  Multiboot 2 module)

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

## Third-party code
[doomgeneric](https://github.com/ozkl/doomgeneric) is vendored under `src/doom/`
as the Doom engine this kernel targets — see `src/doom/LICENSE` (GPL-2.0).

## Assets fetched at build time
`freedoom2.wad` isn't committed to the repo — `docker/scripts/build.sh` fetches
it from the official [Freedoom](https://github.com/freedoom/freedoom) v0.13.0
release on every build, pinned and verified by sha1/sha256 rather than trusted
blindly, and fails the build loudly on a hash mismatch. It's cached at
`build/freedoom2.wad` so a rebuild doesn't re-download it. See
`docs/architecture.md` §7 for the exact pin and how it was verified.

## License
MIT License (at least for now, may change later...?)