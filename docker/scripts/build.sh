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

# Initialized here, ahead of the ring-3 link-target step below, rather than
# at the top of "[3/7] Compile C sources" where it used to live: that step's
# build_ring3_link_target calls append their blob objects to `objs` via
# `objs+=(...)`, and step 3's own `objs=(build/boot.o)` would otherwise run
# *after* them and silently wipe out whatever they had already added.
objs=(build/boot.o)

probe_cflags=("${CFLAGS[@]/-mcmodel=small/-mcmodel=large}")

# Compile+link one ring-3 link target at its real, final ring-3 addresses
# (LIBOS_LAUNCH_CODE_VADDR/_DATA_VADDR) rather than copied there afterward
# like the hand-written .s probes, and objcopy its .text/.data into blobs
# embedded in the kernel image -- the shape libos_c_probe.c's own comment
# describes, shared here so libos_c_probe (SCRUM-173) and libc_shim_probe
# (SCRUM-51) -- and whatever the next such target turns out to be -- can't
# drift on it. mcmodel=large (probe_cflags, set once above) replaces the
# kernel's own mcmodel=small: that model only supports symbols in the first
# 2 GB, and LIBOS_LAUNCH_CODE_VADDR sits inside the 64 TiB LibOS window
# (EXO_USER_VA_BASE, src/exo_syscall.h), nowhere near it.
#
#   $1        name      -- link target name; also its entry symbol
#                          (${name}_main), its bss symbols
#                          (__${name}_bss_start/_end), the _layout.h
#                          basename and the _BSS_LEN macro prefix
#                          (upper-cased), e.g. "libos_c_probe"
#   $2        dir        -- directory holding $name.c(s) and where
#                          ${name}_layout.h is generated
#   $3        extra_inc  -- extra -I path for the compile step ("" for none)
#   $4..      srcs       -- source files to compile and link, IN ORDER: the
#                          first one lands at offset 0 of the .text blob
#                          (ld places each input file's .text contiguously
#                          in command-line order), which is what
#                          libos_build_image() treats as entry_vaddr -- so
#                          the file defining the entry point MUST be listed
#                          first whenever more than one source is passed.
# Appends the resulting code/data blob objects to the global `objs` array.
build_ring3_link_target() {
  local name="$1" dir="$2" extra_inc="$3"
  shift 3
  local srcs=("$@")

  local target_objs=()
  for c in "${srcs[@]}"; do
    local o="build/${name}_$(basename "${c%.c}.o")"
    echo "    CC $(basename "$c") (${name}, ring 3)"
    if [[ -n "$extra_inc" ]]; then
      x86_64-elf-gcc -c "$c" -o "$o" "${probe_cflags[@]}" -I src/ -I "$extra_inc"
    else
      x86_64-elf-gcc -c "$c" -o "$o" "${probe_cflags[@]}" -I src/
    fi
    target_objs+=("$o")
  done

  # ld's expression syntax has no C integer-suffix notion, so LIBOS_LAUNCH_*'s
  # ULL literals (src/libos_launch.h) survive cpp expansion intact and then
  # fail to parse; strip the suffix (unambiguous here -- 'U'/'L' cannot occur
  # inside a hex literal, so every "ULL" in the preprocessed output is a C
  # suffix, never a false match).
  #
  # tests/kernel/ring3_link_target.ld.in is one shared template for every
  # such target; RING3_ENTRY_SYM/_BSS_START_SYM/_BSS_END_SYM are this
  # target's own names, substituted the same way EXO_KERNEL and
  # LIBOS_LAUNCH_CODE_VADDR already are on this same cpp pass.
  x86_64-elf-gcc -E -P -x assembler-with-cpp -I src/ -DEXO_KERNEL \
    -DRING3_ENTRY_SYM="${name}_main" \
    -DRING3_BSS_START_SYM="__${name}_bss_start" \
    -DRING3_BSS_END_SYM="__${name}_bss_end" \
    tests/kernel/ring3_link_target.ld.in | sed 's/ULL//g' > "build/$name.ld"
  x86_64-elf-ld -T "build/$name.ld" -o "build/$name.elf" "${target_objs[@]}"

  x86_64-elf-objcopy -O binary --only-section=.text \
    "build/$name.elf" "build/${name}_code.bin"
  x86_64-elf-objcopy -O binary --only-section=.data \
    "build/$name.elf" "build/${name}_data.bin"

  # .bss carries no file bytes to extract -- its length comes from the
  # linker-defined start/end symbols instead, via the nm_symbol_value()
  # helper step 2b uses to read boot.s's exported flag words.
  local bss_start bss_end bss_len
  bss_start=0x$(nm_symbol_value "build/$name.elf" "__${name}_bss_start")
  bss_end=0x$(nm_symbol_value "build/$name.elf" "__${name}_bss_end")
  if [[ "$bss_start" == "0x" || "$bss_end" == "0x" ]]; then
    echo "    ERROR: $name.elf missing __${name}_bss_start/_end"
    echo "           -- did tests/kernel/ring3_link_target.ld.in's .bss block change?"
    exit 1
  fi
  bss_len=$(( bss_end - bss_start ))

  # Generated into the target's own directory, not build/: a plain
  # #include "${name}_layout.h" from the matching tests/kernel/test_*_k.c
  # (one level up) then resolves via cpp's "search the including file's own
  # directory" rule with no -I flag needed on the shared test-compile loop
  # below -- the same reasoning as keeping these sources out of that loop's
  # glob in the first place.
  local macro_prefix
  macro_prefix=$(echo "$name" | tr '[:lower:]' '[:upper:]')
  printf '#define %s_BSS_LEN %d\n' "$macro_prefix" "$bss_len" \
    > "$dir/${name}_layout.h"

  # objcopy -I binary derives symbol names from the exact path given on the
  # command line; cd into build/ first so they come out as the predictable
  # _binary_${name}_{code,data}_bin_{start,end,size} rather than something
  # that embeds this script's working directory.
  ( cd build && x86_64-elf-objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
      "${name}_code.bin" "${name}_code_blob.o" )
  ( cd build && x86_64-elf-objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
      "${name}_data.bin" "${name}_data_blob.o" )
  objs+=("build/${name}_code_blob.o" "build/${name}_data_blob.o")
}

echo "[2c/7] Build ring-3 WAD/automap viewer (demo)"
# Unconditional -- unlike the TESTING-only targets further down, this is what
# a normal boot (docker-run-kernel) actually launches into ring 3 in place of
# the old ring-0 WAD showcase in src/kernel.c. Runs before "[3/7] Compile C
# sources" below specifically so that step's kernel.c compile can
# #include the libos_wad_viewer_layout.h this generates. Lives in its own
# subdirectory, src/libos_wad_viewer/, so step 3's `for c in src/*.c` loop
# never sees libos_wad_viewer.c and compiles it with -DEXO_KERNEL --
# it calls the !EXO_KERNEL `syscall`-stub side of src/exo_syscall.h
# (exo_get_ticks, exo_kbd_poll, ...), the same reason src/doom/ is excluded
# from that glob. src/wad.c, src/flat.c, src/automap.c, src/fb.c and
# src/fb_console.c have no EXO_KERNEL-only dependencies -- no kmalloc, no
# serial_print, nothing privileged, just pointer arithmetic and framebuffer
# blits -- so they link into this ring-3 target unmodified, the same file
# providing both the ring-0 and ring-3 renderers. libos_wad_viewer.c MUST
# stay first in this list -- see build_ring3_link_target's own comment on
# why source order determines entry_vaddr.
build_ring3_link_target libos_wad_viewer src/libos_wad_viewer "" \
  src/libos_wad_viewer/libos_wad_viewer.c \
  src/wad.c src/flat.c src/automap.c src/fb.c src/fb_console.c src/libos_fb.c


echo "[3/7] Compile C sources"

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
  build_ring3_link_target libos_c_probe tests/kernel/libos_c_probe "" \
    tests/kernel/libos_c_probe/libos_c_probe.c

  echo "[3b2/7] Build libc shim probe (SCRUM-51)"
  # Same mechanism as the libos_c_probe step just above -- a second,
  # independent link target at LIBOS_LAUNCH_CODE_VADDR/_DATA_VADDR -- but
  # this one links in the *actual* libc shim sources (SCRUM-51's real
  # subject) rather than a single throwaway probe function: src/stdlib.c,
  # src/stdio.c, src/string.c, src/ctype.c, plus the LibOS allocators they
  # now sit on, src/libos_heap.c and src/libos_page_alloc.c (SCRUM-37/-38),
  # plus src/libos_fb.c (SCRUM-36), the framebuffer-mapping helper built on
  # exo_fb_acquire/exo_page_map the same way libos_heap.c is built on
  # libos_page_alloc.c.
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
  # already produced, hence build_ring3_link_target's libc_shim_probe_
  # prefix on every object it compiles.
  #
  # libc_shim_probe.c MUST come first in this list -- see
  # build_ring3_link_target's own comment on why source order determines
  # entry_vaddr.
  shim_dir=tests/kernel/libc_shim_probe
  build_ring3_link_target libc_shim_probe "$shim_dir" "$shim_dir" \
    "$shim_dir/libc_shim_probe.c" \
    src/stdlib.c src/stdio.c src/string.c src/ctype.c \
    src/libos_heap.c src/libos_page_alloc.c src/libos_fb.c

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
