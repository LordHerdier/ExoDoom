#!/usr/bin/env bash
set -euo pipefail

cd /work

mkdir -p build/isodir/boot/grub
cp /usr/share/grub/unicode.pf2 build/isodir/boot/grub/

if [[ "${DEBUG:-0}" == "1" ]]; then
  CFLAGS=(-std=gnu99 -ffreestanding -g -O0 -Wall -Wextra -mno-red-zone -mcmodel=small -mno-sse -mno-sse2 -mno-mmx)
else
  CFLAGS=(-std=gnu99 -ffreestanding -O2 -Wall -Wextra -mno-red-zone -mcmodel=small -mno-sse -mno-sse2 -mno-mmx)
fi

if [[ "${TESTING:-0}" == "1" ]]; then
  CFLAGS+=(-DTESTING)
fi

LDFLAGS=(-T src/linker.ld -ffreestanding -O2 -nostdlib -z max-page-size=0x1000)

echo "[1/6] Assemble boot.s"
# RING3_PROBE makes boot.s set the U/S bit through the identity map, without
# which the ring-3 syscall test (tests/kernel/ring3_probe.s) cannot execute a
# single instruction.  It is scoped to test builds on purpose: it opens all of
# physical memory to ring 3, which is exactly what SCRUM-48 and SCRUM-55/-56
# exist to close.  A shipped kernel keeps supervisor-only pages.
BOOT_ASFLAGS=()
if [[ "${TESTING:-0}" == "1" ]]; then
  BOOT_ASFLAGS+=(--defsym RING3_PROBE=1)
fi

x86_64-elf-as "${BOOT_ASFLAGS[@]}" src/boot.s -o build/boot.o

echo "[2/6] Compile C sources"
objs=(build/boot.o)

# -DEXO_KERNEL selects the kernel view of src/exo_syscall.h (numbers, shared
# structs and error codes, no user-side `syscall` stubs).  It lives here rather
# than in a per-file #define so it is guaranteed to precede every transitive
# include of the header in a kernel TU -- a #define after the first include
# would be too late.  tests/kernel/*.c gets it too (see below): those TUs link
# into the kernel and run in ring 0, so the LibOS view is the wrong default
# there -- a stub reaching a real `syscall` with IA32_LSTAR unset would triple
# fault.
for c in src/*.c; do
  o="build/$(basename "${c%.c}.o")"
  echo "    CC $(basename "$c")"
  x86_64-elf-gcc -c "$c" -o "$o" "${CFLAGS[@]}" -DEXO_KERNEL
  objs+=("$o")
done

# Assemble every other src/*.s.  boot.s is excluded because it is handled
# above with its own flags; everything else (isr.s, syscall_entry.s, ...) is
# picked up automatically, the same way src/*.c is.
for s in src/*.s; do
  [[ "$s" == "src/boot.s" ]] && continue
  o="build/$(basename "${s%.s}.o")"
  echo "    AS $(basename "$s")"
  x86_64-elf-as "$s" -o "$o"
  objs+=("$o")
done

if [[ "${TESTING:-0}" == "1" ]]; then
  echo "[2b/6] Compile kernel test sources"
  for c in tests/kernel/*.c; do
    o="build/$(basename "${c%.c}.o")"
    echo "    CC $(basename "$c")"
    # Kernel view by default -- these run in ring 0.  The one TU that needs
    # the LibOS view (test_exo_syscall_k.c, which instantiates the stubs)
    # #undefs it before its first include.
    x86_64-elf-gcc -c "$c" -o "$o" "${CFLAGS[@]}" -I src/ -DEXO_KERNEL
    objs+=("$o")
  done

  # Test-only assembly (ring3_probe.s).  Lives under tests/ rather than src/
  # so it cannot leak into a shipped kernel.
  for s in tests/kernel/*.s; do
    [ -e "$s" ] || continue
    o="build/$(basename "${s%.s}.o")"
    echo "    AS $(basename "$s")"
    x86_64-elf-as "$s" -o "$o"
    objs+=("$o")
  done
fi

echo "[3/6] Link kernel -> build/exodoom"
x86_64-elf-gcc "${LDFLAGS[@]}" -o build/exodoom \
  "${objs[@]}" -lgcc

echo "[4/6] Sanity check multiboot2 header"
if grub-file --is-x86-multiboot2 build/exodoom; then
  echo "    multiboot2 confirmed"
else
  echo "    ERROR: not a valid multiboot2 kernel"
  exit 1
fi

echo "[5/6] Build ISO staging tree"
mkdir -p build/isodir/boot
cp build/exodoom build/isodir/boot/exodoom
cp src/grub.cfg build/isodir/boot/grub/grub.cfg

echo "[6/6] Create ISO -> build/exodoom.iso"
grub-mkrescue -o build/exodoom.iso build/isodir >/dev/null

echo "Done:"
ls -lh build/exodoom build/exodoom.iso
