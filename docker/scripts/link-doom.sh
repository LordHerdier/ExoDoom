#!/usr/bin/env bash
# link-doom.sh — SCRUM-65's acceptance gate.
#
# The ticket's acceptance is "every missing function is either implemented or
# stubbed; linker produces no undefined symbols". This is the check that says
# whether that is true, and keeps saying it.
#
# docker/scripts/build-doom.sh is the COMPILE gate (all 79 files under
# src/doom/ produce a .o). Compiling was never the hard part -- SCRUM-64's own
# comment says so -- because a call that is declared and undefined compiles
# perfectly and only fails when something tries to link it. This script is the
# other half: it compiles the libc shim the way a real ring-3 LibOS target
# would, puts it next to those doom objects, and asks the symbol table what is
# still missing.
#
# ── Why nm rather than an actual ld ────────────────────────────────────
#
# Because there is nothing to link yet. A real link needs an entry point and a
# linker script, and the Doom LibOS target that would supply both is its own
# ticket ("link Doom + LibOS + libc shim into a single exodoom ELF", Sprint
# 7). Asking `nm` for the difference between what the objects reference and
# what they define answers exactly the question this ticket poses, without
# inventing the layout of a binary nobody has designed yet -- and it gives a
# far better error message than ld would, because it can name every missing
# symbol at once instead of stopping at the first.
set -uo pipefail

cd /work
mkdir -p build/doom build/shim

# Same flags as build-doom.sh. -mno-sse is absent for the same reason it is
# absent there: m_config.c's M_GetFloatVariable() returns a float and the
# x86_64 SysV ABI has no SSE-free encoding for that (SCRUM-177).
DOOM_CFLAGS=(-std=gnu99 -ffreestanding -O2 -w -mno-red-zone -mcmodel=small
             -mno-mmx -I src/doom -I src)

# The libc shim, compiled the way build.sh's build_ring3_link_target() does:
#
#   NO -DEXO_KERNEL -- this is the LibOS view. That flag is what switches
#     malloc onto libos_heap_alloc, printf's sink onto exo_serial_write, and
#     (SCRUM-65) enables the %f and atof paths that need SSE.
#   -mcmodel=large -- LIBOS_LAUNCH_CODE_VADDR is 64 TiB up, nowhere near the
#     2 GB that mcmodel=small can address.
#
# Getting either wrong here would gate the wrong build: the kernel-side
# compile of these same files resolves a different set of symbols.
SHIM_CFLAGS=(-std=gnu99 -ffreestanding -O2 -Wall -Wextra -mno-red-zone
             -mcmodel=large -mno-mmx -I src)

SHIM_SOURCES=(
  src/stdio.c
  src/stdlib.c
  src/string.c
  src/ctype.c
  src/errno.c
  src/fpconv.c
  src/doom_net_stub.c
  src/doomgeneric_exo.c
  src/libos_heap.c
  src/libos_page_alloc.c
  src/libos_fb.c
)

# Symbols that are allowed to remain undefined, each with the ticket that
# owns it. This list is the point of the whole script: it is not "things we
# gave up on", it is the boundary between SCRUM-65's scope (libc) and the
# tickets on either side of it. A symbol that is NOT libc and NOT on this
# list fails the build, and so does a libc symbol -- there is no third
# category.
#
# docs/libc_audit.md classifies the DG_* callbacks as PLATFORM rather than
# libc for exactly this reason: they are the port's own six functions, not
# anything the C standard owes Doom.
declare -A ALLOWED=(
  [DG_Init]="SCRUM-73 -- mounts the multiboot-module WAD"
  [DG_DrawFrame]="Sprint 8 -- needs exo_fb_acquire + the BGRX blit"
  [DG_GetKey]="Sprint 8 -- needs exo_kbd_poll + keycode translation"
  [DG_SetWindowTitle]="Sprint 8 -- a no-op or a serial line"
  [_GLOBAL_OFFSET_TABLE_]="not a symbol -- ld synthesises this itself"
)

echo "[1/3] Compile vendored Doom engine"
fail=0
for c in src/doom/*.c; do
  name="$(basename "${c%.c}")"
  if ! x86_64-elf-gcc -c "$c" -o "build/doom/${name}.o" "${DOOM_CFLAGS[@]}" \
         2>"build/doom/${name}.log"; then
    echo "    FAIL $(basename "$c")"
    fail=$((fail + 1))
  fi
done
if [[ $fail -gt 0 ]]; then
  echo "    $fail file(s) failed to compile -- see build/doom/<name>.log"
  echo "    (docker/scripts/build-doom.sh is the gate for this; fix there first)"
  exit 1
fi
echo "    79 objects"

echo "[2/3] Compile libc shim for the ring-3 LibOS view"
for c in "${SHIM_SOURCES[@]}"; do
  name="$(basename "${c%.c}")"
  echo "    CC $(basename "$c")"
  if ! x86_64-elf-gcc -c "$c" -o "build/shim/${name}.o" "${SHIM_CFLAGS[@]}"; then
    echo "    ERROR: the libc shim itself does not compile for ring 3."
    exit 1
  fi
done

echo "[3/3] Resolve symbols"

# Defined: anything with a non-'U' type. W (weak) and V (weak object) count
# as defined -- a weak definition satisfies a reference.
x86_64-elf-nm --defined-only build/doom/*.o build/shim/*.o 2>/dev/null \
  | awk '$2 ~ /[TDBRGWVtdbrs]/ { print $3 }' | sort -u > build/doom_defined.txt
x86_64-elf-nm -u build/doom/*.o build/shim/*.o 2>/dev/null \
  | awk '{ print $2 }' | grep -v '^$' | sort -u > build/doom_undefined.txt

comm -23 build/doom_undefined.txt build/doom_defined.txt > build/doom_missing.txt

unexpected=0
while read -r sym; do
  [[ -z "$sym" ]] && continue
  if [[ -v ALLOWED[$sym] ]]; then
    echo "    expected: $sym  (${ALLOWED[$sym]})"
  else
    echo "    MISSING:  $sym"
    unexpected=$((unexpected + 1))
  fi
done < build/doom_missing.txt

# Also report the reverse: an allowlist entry that is no longer missing means
# someone implemented it and forgot to prune this list. Not an error -- the
# build should not break because a ticket landed -- but it should be visible,
# or the list rots into a set of exemptions nobody rechecks.
while read -r sym _; do
  [[ -z "$sym" ]] && continue
  if ! grep -qx "$sym" build/doom_missing.txt; then
    echo "    NOTE: '$sym' is in the allowlist but is now defined -- prune it."
  fi
done < <(for k in "${!ALLOWED[@]}"; do echo "$k"; done)

echo ""
if [[ $unexpected -gt 0 ]]; then
  echo "$unexpected symbol(s) still unresolved beyond the known platform work."
  echo ""
  echo "SCRUM-65's acceptance is that no libc symbol is left undefined."
  echo "If a symbol above is genuinely not libc and belongs to another"
  echo "ticket, add it to ALLOWED in this script WITH that ticket's number --"
  echo "do not widen the list silently."
  exit 1
fi

echo "All libc symbols resolved; only the known platform callbacks remain."
exit 0
