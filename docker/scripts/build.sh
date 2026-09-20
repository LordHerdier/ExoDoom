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

objs=(build/boot.o)

probe_cflags=("${CFLAGS[@]/-mcmodel=small/-mcmodel=large}" -fno-toplevel-reorder)

# -fno-toplevel-reorder: libos_build_image() always treats byte 0 of the
# code blob as the entry point -- no ELF symbol lookup, no e_entry, just the
# base address (see its own comment in src/libos_launch.h and
# build_ring3_link_target's $4 doc below). ld places each *object file's*
# .text contiguously in command-line order, but WITHIN one object, GCC's
# default -ftoplevel-reorder (on by default from -O1 up) is free to emit
# top-level definitions -- static functions included -- in any order it
# finds convenient for code locality, regardless of source order. Discovered
# the hard way building the SCRUM-110 shell target: a second, small static
# helper compiled to a standalone function (not inlined, since it had two
# call sites) ended up placed by GCC *before* shell_main() in the object's
# .text, even with shell_main() written first in the source -- so the
# launched context executed that helper's bytes as its entry and immediately
# page-faulted on an early write into the (non-writable) code region.
# -fno-toplevel-reorder is the actual, verified fix (confirmed via
# `x86_64-elf-nm -n` on the linked .elf showing the entry function's symbol
# at the lowest address after adding this flag) -- an earlier attempt with
# -fno-reorder-functions alone did NOT fix it, since that flag governs a
# different, profile-guided hot/cold partitioning pass, not this one.
# libos_c_probe.c and libc_shim_probe.c never hit this because each keeps
# its entry function as the ONLY function in its own object; this flag lifts
# that as a hard requirement for every ring-3 link target instead of relying
# on each new one to rediscover it independently.

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
# Defined here, ahead of step 3's C compile loop below, rather than after it
# as originally written: SCRUM-110's shell target (called immediately below)
# must be built and its src/shell/shell_layout.h generated *before*
# src/kernel.c compiles, since that file #includes it -- unlike
# libos_c_probe/libc_shim_probe (called further down, inside the TESTING=1
# block), which nothing in step 3 depends on.
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

echo "[2c/7] Build shell LibOS (SCRUM-110)"
# Unlike libos_c_probe/libc_shim_probe (built further down, inside the
# TESTING=1 block), this target is built UNCONDITIONALLY, and ahead of step
# 3's C compile loop -- because kernel_main() launches it on every normal
# boot (src/kernel.c), not just under a test harness, and src/kernel.c
# itself #includes the generated src/shell/shell_layout.h this call
# produces. Same build_ring3_link_target mechanism otherwise:
# src/shell/shell_main.c is its own subdirectory (like src/doom/)
# specifically so step 3's plain `src/*.c` glob below never sees it -- it
# must be compiled once, without -DEXO_KERNEL, for the real ring-3 syscall
# stubs, not twice (kernel view + LibOS view) the way every ordinary
# src/*.c file is.
#
# src/fb.c and src/fb_console.c are pulled in unmodified -- both are
# already framebuffer-address-agnostic freestanding C with no EXO_KERNEL
# gate, so the same text console logic src/kernel.c's own boot banner uses
# now also runs as compiled ring-3 code against the shell's own mapped
# framebuffer. src/libos_fb.c (SCRUM-36) composes
# exo_fb_acquire()/exo_page_map() into the single libos_fb_map() call
# shell_main.c uses to get that mapping.
#
# shell_main.c MUST come first in this list -- see build_ring3_link_target's
# own comment on why source order determines entry_vaddr.
build_ring3_link_target shell src/shell "" \
  src/shell/shell_main.c src/fb.c src/fb_console.c src/libos_fb.c

echo "[2d/7] Build WAD/flat/automap viewer LibOS (SCRUM-178)"
# Same reasoning and same mechanism as the shell step just above: built
# UNCONDITIONALLY and ahead of step 3's C compile loop, because
# src/syscall_launch.c (the kernel-side #21 handler that launches this
# LibOS from the shell's "wadview" command) #includes the generated
# src/libos_wad_viewer/libos_wad_viewer_layout.h this call produces.
# src/libos_wad_viewer/ is its own subdirectory for the same reason
# src/shell/ is: step 3's plain `src/*.c` glob below must never compile
# libos_wad_viewer.c with -DEXO_KERNEL.
#
# src/wad.c/src/flat.c/src/automap.c are the engine (WAD parsing, flat
# texture blitting, automap line rendering); src/fb.c/src/fb_console.c/
# src/libos_fb.c are the same framebuffer/text-console/mapping code the
# shell target above already links in unmodified. libos_wad_viewer.c MUST
# come first in this list --
# see build_ring3_link_target's own comment on why source order determines
# entry_vaddr (this target also carries the
# __attribute__((section(".text.entry"))) belt-and-suspenders fix its own
# top comment describes, for the same reason the shell target's comment
# gives for keeping shell_main() textually first too). The same ordering
# also fixes this target's params struct at offset 0 of .data (SCRUM-175):
# libos_wad_viewer.c's g_wad_params is that TU's first global, and being
# first in this source list is what puts it first in link order too -- see
# libos_launch_patch_params()'s comment in src/libos_launch.h.
build_ring3_link_target libos_wad_viewer src/libos_wad_viewer "" \
  src/libos_wad_viewer/libos_wad_viewer.c src/wad.c src/flat.c src/automap.c \
  src/fb.c src/fb_console.c src/libos_fb.c

echo "[2e/7] Build clock demo LibOS (SCRUM-168)"
# Same reasoning and same mechanism as the shell/WAD-viewer steps just
# above: built UNCONDITIONALLY and ahead of step 3's C compile loop, because
# src/syscall_launch.c (the kernel-side #22 handler that launches this
# LibOS from the shell's "clock" command) #includes the generated
# src/libos_clock/libos_clock_layout.h this call produces. src/libos_clock/
# is its own subdirectory for the same reason src/shell/ and
# src/libos_wad_viewer/ are: step 3's plain `src/*.c` glob below must never
# compile libos_clock.c with -DEXO_KERNEL.
#
# src/fb.c/src/fb_console.c/src/libos_fb.c are the same framebuffer/
# text-console/mapping code the shell and WAD-viewer targets above already
# link in unmodified. libos_clock.c MUST come first in this list -- see
# build_ring3_link_target's own comment on why source order determines
# entry_vaddr (this target also carries the
# __attribute__((section(".text.entry"))) belt-and-suspenders fix, for the
# same reason the WAD-viewer target's own comment gives).
build_ring3_link_target libos_clock src/libos_clock "" \
  src/libos_clock/libos_clock.c src/fb.c src/fb_console.c src/libos_fb.c

echo "[2f/7] Build Snake LibOS (SCRUM-182)"
# Same reasoning and same mechanism as the WAD viewer step just above: built
# UNCONDITIONALLY and ahead of step 3's C compile loop, because
# src/syscall_launch.c (the kernel-side #22 handler that launches this LibOS
# from the shell's "snake" command) #includes the generated
# src/libos_snake/libos_snake_layout.h this call produces. src/libos_snake/
# is its own subdirectory for the same reason src/libos_wad_viewer/ is: step
# 3's plain `src/*.c` glob below must never compile libos_snake.c with
# -DEXO_KERNEL.
#
# Simpler than the WAD viewer target: no params struct, so no equivalent of
# g_wad_params's "must be the first global in the first-listed source file"
# requirement -- only entry-point placement (.text offset 0) needs
# libos_snake.c listed first, matching every other ring-3 target's
# convention (and its own __attribute__((section(".text.entry"))) belt-and-
# suspenders fix, same reasoning as libos_wad_viewer.c's own comment).
#
# src/fb.c/src/fb_console.c/src/libos_fb.c are the same framebuffer/text-
# console/mapping code the shell and WAD viewer targets above already link
# in unmodified.
build_ring3_link_target libos_snake src/libos_snake "" \
  src/libos_snake/libos_snake.c src/fb.c src/fb_console.c src/libos_fb.c

echo "[3/7] Compile C sources"

# -DEXO_KERNEL selects the kernel view of src/exo_syscall.h (numbers, shared
# structs and error codes, no user-side `syscall` stubs).  It lives here rather
# than in a per-file #define so it is guaranteed to precede every transitive
# include of the header in a kernel TU -- a #define after the first include
# would be too late.  tests/kernel/*.c gets it too (see below): those TUs link
# into the kernel and run in ring 0, so the LibOS view is the wrong default
# there -- a stub reaching a real `syscall` with IA32_LSTAR unset would triple
# fault.
#
# -I src (SCRUM-74): src/doomgeneric_exo.c is ExoDoom's doomgeneric platform
# file, and it includes doomgeneric's own header for the DG_* prototypes
# rather than restating them -- src/doom/doomgeneric.h in turn includes
# <stdlib.h>, which only resolves to the shim's src/stdlib.h with src/ on the
# angle-bracket path. Every other src/*.c reaches its headers with quoted
# includes and is unaffected; src/ holds no header that shadows one of GCC's
# freestanding four (stddef/stdint/stdarg/limits), so nothing is redirected
# by this that was not already coming from src/.
for c in src/*.c; do
  o="build/$(basename "${c%.c}.o")"
  echo "    CC $(basename "$c")"
  x86_64-elf-gcc -c "$c" -o "$o" "${CFLAGS[@]}" -I src -DEXO_KERNEL
  objs+=("$o")
done

# Assemble every other src/*.s.  boot.s is excluded because it is handled
# above with its own flags; everything else (isr.s, syscall_entry.s, ...) is
# picked up automatically, the same way src/*.c is.
#
# Run through the C preprocessor first (`-x assembler-with-cpp`, same as the
# tests/kernel/*.s loop below), rather than handed to `as` directly (SCRUM-108):
# libos_enter.s and context_switch.s #include libos_launch.h so both share
# LIBOS_LAUNCH_RFLAGS/_USER_SS/_USER_CS with the C side instead of a
# hand-copied literal that can drift from it, the same reason the test-probe
# loop already does this. A file with nothing to include still assembles
# fine: cpp with no macros used is a no-op.
for s in src/*.s; do
  [[ "$s" == "src/boot.s" ]] && continue
  pp="build/$(basename "${s%.s}.pp.s")"
  o="build/$(basename "${s%.s}.o")"
  echo "    AS $(basename "$s")"
  x86_64-elf-gcc -E -P -x assembler-with-cpp -I src -DEXO_KERNEL "$s" -o "$pp"
  x86_64-elf-as "$pp" -o "$o"
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
  # src/errno.c and src/fpconv.c are SCRUM-65 additions to this list, and
  # they are here because src/stdio.c and src/stdlib.c grew real dependencies
  # on them rather than because anything in the probe calls them directly:
  # fopen/fseek/ftell set errno (which src/errno.h resolves to exo_errno), and
  # %f / atof route through exo_fmt_f64 / exo_parse_f64. Leaving them out is
  # not a compile error anywhere -- it is an undefined reference at THIS
  # target's link step, which is how CI found it.
  shim_dir=tests/kernel/libc_shim_probe
  build_ring3_link_target libc_shim_probe "$shim_dir" "$shim_dir" \
    "$shim_dir/libc_shim_probe.c" \
    src/stdlib.c src/stdio.c src/string.c src/ctype.c \
    src/errno.c src/fpconv.c \
    src/libos_heap.c src/libos_page_alloc.c src/libos_fb.c

  echo "[3b3/7] Compile Doom's reference trig tables (SCRUM-41)"
  # The ONE file under src/doom/ that the kernel image links, and only in a
  # TESTING build.  tests/kernel/test_fixed_math_k.c is SCRUM-41's acceptance
  # test: it regenerates finesine/finecosine/finetangent/tantoangle through
  # src/fixed_math.c and compares all 24,577 entries against the vendored
  # chocolate-doom data, which is what the ticket means by "verified against
  # reference".  Hand-copying a few spot values into the test instead would
  # verify the shape of the curve and nothing else.
  #
  # This is a deliberate exception to "nothing in src/doom/ links into
  # build/exodoom", and a narrow one: tables.c is pure const integer arrays
  # with no libc and no engine dependency, so it needs none of the shim the
  # other 78 files are still waiting on (docs/libc_audit.md), and it compiles
  # clean under the kernel's own -mno-sse CFLAGS.  It is compiled here rather
  # than left to step 3's `src/*.c` glob because that glob deliberately does
  # not descend into src/doom/, and it stays inside the TESTING block so a
  # shipped kernel still links nothing from the vendored tree.
  #
  # -I src/doom is what step 3's loop does not pass: tables.c includes the
  # engine's own tables.h/m_fixed.h/doomtype.h, which in turn reach the libc
  # shim headers via -I src.
  echo "    CC tables.c (doom reference data)"
  x86_64-elf-gcc -c src/doom/tables.c -o build/doom_tables.o \
    "${CFLAGS[@]}" -I src/ -I src/doom -DEXO_KERNEL
  objs+=("build/doom_tables.o")

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
