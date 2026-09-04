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

echo "[1b/6] Verify page-table protection"
# Assert the U/S gate landed the way this build intends, rather than trusting
# that it did.  boot.s exports the two flag words it actually used as absolute
# symbols, so this reads the real constants -- not a restatement of them that
# could drift, and not instruction encodings that could change.
#
# Both directions are worth checking:
#   - a shipped kernel with U/S set would expose all kernel memory to ring 3;
#   - a test kernel *without* it triple-faults the moment the ring-3 probe is
#     entered, which surfaces as an unexplained CI timeout rather than a
#     failing assertion.
pt_flag() {
  x86_64-elf-nm build/boot.o | awk -v s="$1" '$3 == s { print $1 }'
}

pt_link="$(pt_flag boot_pt_link_flags)"
pt_leaf="$(pt_flag boot_pt_leaf_flags)"

if [[ -z "$pt_link" || -z "$pt_leaf" ]]; then
  echo "    ERROR: boot.s no longer exports boot_pt_link_flags/boot_pt_leaf_flags."
  echo "           These guard the ring-3 U/S gate -- restore them, do not"
  echo "           delete this check.  See docs/memory.md."
  exit 1
fi

if [[ "${TESTING:-0}" == "1" ]]; then
  want_link=0000000000000007; want_leaf=0000000000000087
  want_desc="user-accessible (ring-3 probe)"
else
  want_link=0000000000000003; want_leaf=0000000000000083
  want_desc="supervisor-only"
fi

if [[ "$pt_link" != "$want_link" || "$pt_leaf" != "$want_leaf" ]]; then
  echo "    ERROR: identity map has the wrong protection for this build."
  echo "           expected ($want_desc): link=0x${want_link: -2} leaf=0x${want_leaf: -2}"
  echo "           got:                     link=0x${pt_link: -2} leaf=0x${pt_leaf: -2}"
  if [[ "${TESTING:-0}" != "1" ]]; then
    echo
    echo "           A shipped kernel must NOT set the U/S bit: it would make"
    echo "           all 4 GB of the identity map readable and writable from"
    echo "           ring 3, including kernel text and the page tables."
    echo "           RING3_PROBE belongs to TESTING=1 builds only."
  fi
  exit 1
fi

echo "    identity map is $want_desc (link=0x${pt_link: -2} leaf=0x${pt_leaf: -2})"

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
