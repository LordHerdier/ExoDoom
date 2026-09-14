#!/usr/bin/env bash
set -euo pipefail

cd /work

mkdir -p build/isodir/boot/grub
cp /usr/share/grub/unicode.pf2 build/isodir/boot/grub/

echo "[1/7] Fetch freedoom2 IWAD"
# Fetched at build time and pinned by sha1 rather than committed (SCRUM-164)
# -- avoids a 29 MB binary in git and makes swapping WADs a URL+hash edit
# here. build/ is bind-mounted from the host (see Makefile), so a valid
# download is reused across builds instead of re-fetched every time.
#
# Official Freedoom v0.13.0 release (github.com/freedoom/freedoom).
# ZIP_SHA256 was checked against the project's GPG-signed
# freedoom-0.13.0-CHECKSUM before pinning; WAD_SHA1 is freedoom2.wad's own
# hash once extracted from that verified zip. Re-verify both if this pin
# ever changes.
ZIP_URL="https://github.com/freedoom/freedoom/releases/download/v0.13.0/freedoom-0.13.0.zip"
ZIP_SHA256="3f9b264f3e3ce503b4fb7f6bdcb1f419d93c7b546f4df3e874dd878db9688f59"
WAD_SHA1="975f781e6d801c0a23e3caa33f70493efe68a880"
WAD_PATH="build/freedoom2.wad"
ZIP_PATH="build/freedoom-0.13.0.zip"

mkdir -p build
if [[ -f "$WAD_PATH" ]] && echo "${WAD_SHA1}  ${WAD_PATH}" | sha1sum -c - >/dev/null 2>&1; then
  echo "    cached at $WAD_PATH, sha1 verified"
else
  echo "    downloading freedoom-0.13.0.zip..."
  curl -fsSL "$ZIP_URL" -o "$ZIP_PATH"
  if ! echo "${ZIP_SHA256}  ${ZIP_PATH}" | sha256sum -c -; then
    echo "    ERROR: freedoom-0.13.0.zip sha256 mismatch"
    rm -f "$ZIP_PATH"
    exit 1
  fi
  unzip -p "$ZIP_PATH" "freedoom-0.13.0/freedoom2.wad" > "$WAD_PATH"
  rm -f "$ZIP_PATH"
  if ! echo "${WAD_SHA1}  ${WAD_PATH}" | sha1sum -c -; then
    echo "    ERROR: freedoom2.wad sha1 mismatch"
    echo "           got:  $(sha1sum "$WAD_PATH" | awk '{print $1}')"
    echo "           want: ${WAD_SHA1}"
    rm -f "$WAD_PATH"
    exit 1
  fi
  echo "    downloaded and verified"
fi

if [[ "${DEBUG:-0}" == "1" ]]; then
  CFLAGS=(-std=gnu99 -ffreestanding -g -O0 -Wall -Wextra -mno-red-zone -mcmodel=small -mno-sse -mno-sse2 -mno-mmx)
else
  CFLAGS=(-std=gnu99 -ffreestanding -O2 -Wall -Wextra -mno-red-zone -mcmodel=small -mno-sse -mno-sse2 -mno-mmx)
fi

if [[ "${TESTING:-0}" == "1" ]]; then
  CFLAGS+=(-DTESTING)
fi

LDFLAGS=(-T src/linker.ld -ffreestanding -O2 -nostdlib -z max-page-size=0x1000)

echo "[2/7] Assemble boot.s"
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

echo "[2b/7] Verify page-table protection"
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
# Read one absolute symbol's value out of an object/ELF via nm -- reused
# below (step 3b) to read libos_c_probe.elf's __libos_c_probe_bss_start/_end,
# the same "trust the real exported constant, not a restatement of it"
# reasoning as here.
nm_symbol_value() {
  x86_64-elf-nm "$1" | awk -v s="$2" '$3 == s { print $1 }'
}

pt_link="$(nm_symbol_value build/boot.o boot_pt_link_flags)"
pt_leaf="$(nm_symbol_value build/boot.o boot_pt_leaf_flags)"

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

echo "[3/7] Compile C sources"
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
  echo "[3b/7] Build LibOS C probe (SCRUM-173)"
  # tests/kernel/libos_c_probe/ is its own directory, not tests/kernel/*.c,
  # specifically so the shared test-compile loop below (step 3c) never needs
  # to know this file exists -- CLAUDE.md's "no build-script change needed to
  # add a test file" promise stays true for that loop; this probe simply
  # isn't a file the loop's glob ever sees, rather than an exception it has
  # to special-case.
  #
  # Compiled and linked at its real, final ring-3 addresses
  # (LIBOS_LAUNCH_CODE_VADDR/_DATA_VADDR) rather than copied there
  # afterward like the hand-written .s probes -- see libos_c_probe.c's own
  # comment for why that makes ordinary compiler-generated addressing
  # correct with no PIC workaround. mcmodel=large replaces the kernel's own
  # mcmodel=small: that model only supports symbols in the first 2 GB, and
  # LIBOS_LAUNCH_CODE_VADDR sits inside the 64 TiB LibOS window
  # (EXO_USER_VA_BASE, src/exo_syscall.h), nowhere near it.
  probe_dir=tests/kernel/libos_c_probe
  probe_cflags=("${CFLAGS[@]/-mcmodel=small/-mcmodel=large}")
  x86_64-elf-gcc -c "$probe_dir/libos_c_probe.c" -o build/libos_c_probe.o \
    "${probe_cflags[@]}" -I src/

  # ld's expression syntax has no C integer-suffix notion, so LIBOS_LAUNCH_*'s
  # ULL literals (src/libos_launch.h) survive cpp expansion intact and then
  # fail to parse; strip the suffix (unambiguous here -- 'U'/'L' cannot occur
  # inside a hex literal, so every "ULL" in the preprocessed output is a C
  # suffix, never a false match).
  x86_64-elf-gcc -E -P -x assembler-with-cpp -I src/ -DEXO_KERNEL \
    "$probe_dir/libos_c_probe.ld.in" | sed 's/ULL//g' > build/libos_c_probe.ld
  x86_64-elf-ld -T build/libos_c_probe.ld -o build/libos_c_probe.elf \
    build/libos_c_probe.o

  x86_64-elf-objcopy -O binary --only-section=.text \
    build/libos_c_probe.elf build/libos_c_probe_code.bin
  x86_64-elf-objcopy -O binary --only-section=.data \
    build/libos_c_probe.elf build/libos_c_probe_data.bin

  # .bss carries no file bytes to extract -- its length comes from the
  # linker-defined start/end symbols instead, via the same nm_symbol_value()
  # helper step 2b uses to read boot.s's exported flag words.
  bss_start=0x$(nm_symbol_value build/libos_c_probe.elf __libos_c_probe_bss_start)
  bss_end=0x$(nm_symbol_value build/libos_c_probe.elf __libos_c_probe_bss_end)
  if [[ "$bss_start" == "0x" || "$bss_end" == "0x" ]]; then
    echo "    ERROR: libos_c_probe.elf missing __libos_c_probe_bss_start/_end"
    echo "           -- did libos_c_probe.ld.in's .bss block change?"
    exit 1
  fi
  bss_len=$(( bss_end - bss_start ))

  # Generated into the probe's own directory, not build/: a plain
  # #include "libos_c_probe_layout.h" from test_libos_c_probe_k.c
  # (tests/kernel/, one level up) then resolves via cpp's "search the
  # including file's own directory" rule with no -I flag needed on the
  # shared test-compile loop below -- the same reasoning as keeping
  # libos_c_probe.c out of that loop's glob in the first place.
  printf '#define LIBOS_C_PROBE_BSS_LEN %d\n' "$bss_len" \
    > "$probe_dir/libos_c_probe_layout.h"

  # objcopy -I binary derives symbol names from the exact path given on the
  # command line; cd into build/ first so they come out as the predictable
  # _binary_libos_c_probe_{code,data}_bin_{start,end,size} rather than
  # something that embeds this script's working directory.
  ( cd build && x86_64-elf-objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
      libos_c_probe_code.bin libos_c_probe_code_blob.o )
  ( cd build && x86_64-elf-objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
      libos_c_probe_data.bin libos_c_probe_data_blob.o )
  objs+=(build/libos_c_probe_code_blob.o build/libos_c_probe_data_blob.o)

  echo "[3b2/7] Build libc shim probe (SCRUM-51)"
  # Same mechanism as the libos_c_probe step just above -- a second,
  # independent link target at LIBOS_LAUNCH_CODE_VADDR/_DATA_VADDR -- but
  # this one links in the *actual* libc shim sources (SCRUM-51's real
  # subject) rather than a single throwaway probe function: src/stdlib.c,
  # src/stdio.c, src/string.c, src/ctype.c, plus the LibOS allocators they
  # now sit on, src/libos_heap.c and src/libos_page_alloc.c (SCRUM-37/-38).
  #
  # Compiled WITHOUT -DEXO_KERNEL -- unlike every other src/*.c compile in
  # this script (step 3 above always passes it) -- which is what flips
  # stdlib.c's malloc/free/realloc onto libos_heap_alloc/_free/_realloc,
  # stdio.c's printf onto exo_serial_write(), and libos_page_alloc.c's
  # do_page_*() onto the real exo_page_alloc()/exo_page_map()/... `syscall`
  # stubs instead of the in-process exo_syscall_dispatch() call those files
  # use when built into the kernel for their own SCRUM-37/-38 unit tests
  # (see each file's own #ifdef EXO_KERNEL comment). Each source needs its
  # own object file distinct from the kernel-side build/<name>.o step 3
  # already produced, hence the libc_shim_ prefix below.
  shim_dir=tests/kernel/libc_shim_probe
  # libc_shim_probe.c MUST come first: libos_build_image() always sets
  # entry_vaddr to LIBOS_LAUNCH_CODE_VADDR itself (src/libos_launch.c) --
  # the base of the whole code blob, not a symbol looked up by name -- the
  # same convention libos_c_probe.c relies on by being the *only* object in
  # its link. With more than one object file, ld places each input file's
  # .text contiguously in command-line order, so whichever file is linked
  # first lands at offset 0 of the blob, i.e. at LIBOS_LAUNCH_CODE_VADDR --
  # and that has to be libc_shim_probe_main, or iretq lands on whatever
  # stdlib.c/stdio.c function happened to compile first instead.
  shim_srcs=("$shim_dir/libc_shim_probe.c" \
             src/stdlib.c src/stdio.c src/string.c src/ctype.c \
             src/libos_heap.c src/libos_page_alloc.c)
  shim_objs=()
  for c in "${shim_srcs[@]}"; do
    o="build/libc_shim_$(basename "${c%.c}.o")"
    echo "    CC $(basename "$c") (libc shim, ring 3)"
    x86_64-elf-gcc -c "$c" -o "$o" "${probe_cflags[@]}" -I src/ -I "$shim_dir"
    shim_objs+=("$o")
  done

  x86_64-elf-gcc -E -P -x assembler-with-cpp -I src/ -DEXO_KERNEL \
    "$shim_dir/libc_shim_probe.ld.in" | sed 's/ULL//g' > build/libc_shim_probe.ld
  x86_64-elf-ld -T build/libc_shim_probe.ld -o build/libc_shim_probe.elf \
    "${shim_objs[@]}"

  x86_64-elf-objcopy -O binary --only-section=.text \
    build/libc_shim_probe.elf build/libc_shim_probe_code.bin
  x86_64-elf-objcopy -O binary --only-section=.data \
    build/libc_shim_probe.elf build/libc_shim_probe_data.bin

  shim_bss_start=0x$(nm_symbol_value build/libc_shim_probe.elf __libc_shim_probe_bss_start)
  shim_bss_end=0x$(nm_symbol_value build/libc_shim_probe.elf __libc_shim_probe_bss_end)
  if [[ "$shim_bss_start" == "0x" || "$shim_bss_end" == "0x" ]]; then
    echo "    ERROR: libc_shim_probe.elf missing __libc_shim_probe_bss_start/_end"
    echo "           -- did libc_shim_probe.ld.in's .bss block change?"
    exit 1
  fi
  shim_bss_len=$(( shim_bss_end - shim_bss_start ))

  printf '#define LIBC_SHIM_PROBE_BSS_LEN %d\n' "$shim_bss_len" \
    > "$shim_dir/libc_shim_probe_layout.h"

  ( cd build && x86_64-elf-objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
      libc_shim_probe_code.bin libc_shim_probe_code_blob.o )
  ( cd build && x86_64-elf-objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
      libc_shim_probe_data.bin libc_shim_probe_data_blob.o )
  objs+=(build/libc_shim_probe_code_blob.o build/libc_shim_probe_data_blob.o)

  echo "[3c/7] Compile kernel test sources"
  for c in tests/kernel/*.c; do
    o="build/$(basename "${c%.c}.o")"
    echo "    CC $(basename "$c")"
    # Kernel view by default -- these run in ring 0.  The one TU that needs
    # the LibOS view (test_exo_syscall_k.c, which instantiates the stubs)
    # #undefs it before its first include.
    x86_64-elf-gcc -c "$c" -o "$o" "${CFLAGS[@]}" -I src/ -DEXO_KERNEL
    objs+=("$o")
  done

  # Test-only assembly (ring3_probe.s, libos_launch_probe.s, ...).  Lives
  # under tests/ rather than src/ so it cannot leak into a shipped kernel.
  #
  # Run through the C preprocessor first (`-x assembler-with-cpp`, which
  # predefines __ASSEMBLER__) rather than handed to `as` directly: probes
  # that need a syscall number or a LibOS-window constant #include the real
  # kernel header (exo_syscall.h / libos_launch.h) and reference its actual
  # macro instead of a hand-copied literal that can silently drift from it
  # (SCRUM-50). -DEXO_KERNEL matches every other kernel-side TU so the
  # LibOS-only stub block those headers guard with `#ifndef EXO_KERNEL`
  # -- plain C with inline asm inside, which `as` cannot parse -- is skipped
  # here exactly as it is for a normal kernel .c file. A probe with nothing
  # to include still assembles fine: cpp with no macros used is a no-op.
  for s in tests/kernel/*.s; do
    [ -e "$s" ] || continue
    pp="build/$(basename "${s%.s}.pp.s")"
    o="build/$(basename "${s%.s}.o")"
    echo "    AS $(basename "$s")"
    x86_64-elf-gcc -E -P -x assembler-with-cpp -I src/ -DEXO_KERNEL "$s" -o "$pp"
    x86_64-elf-as "$pp" -o "$o"
    objs+=("$o")
  done
fi

echo "[4/7] Link kernel -> build/exodoom"
x86_64-elf-gcc "${LDFLAGS[@]}" -o build/exodoom \
  "${objs[@]}" -lgcc

echo "[5/7] Sanity check multiboot2 header"
if grub-file --is-x86-multiboot2 build/exodoom; then
  echo "    multiboot2 confirmed"
else
  echo "    ERROR: not a valid multiboot2 kernel"
  exit 1
fi

echo "[6/7] Build ISO staging tree"
mkdir -p build/isodir/boot
cp build/exodoom build/isodir/boot/exodoom
cp build/freedoom2.wad build/isodir/boot/freedoom2.wad
cp src/grub.cfg build/isodir/boot/grub/grub.cfg

echo "[7/7] Create ISO -> build/exodoom.iso"
grub-mkrescue -o build/exodoom.iso build/isodir >/dev/null

echo "Done:"
ls -lh build/exodoom build/exodoom.iso
