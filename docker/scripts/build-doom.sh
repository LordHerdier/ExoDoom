#!/usr/bin/env bash
# build-doom.sh — compile pass over the vendored doomgeneric core engine
# (src/doom/*.c) with the same freestanding cross-compiler used for the kernel.
#
# As of SCRUM-64 all 79 files compile with zero errors, and this script exits
# non-zero if that ever stops being true — it is a gate, not a status report.
# Before SCRUM-64 only 2 of 79 compiled and it always exited 0.
#
# What it is NOT: a link. Nothing here is linked into build/exodoom, and a good
# deal of what these objects reference has no definition anywhere in the tree —
# every stdio file operation, `mkdir`, `strdup`, `atof`, `exit`, `calloc` and
# the rest. Compiling is genuinely the milestone SCRUM-64 asked for, but "79
# passed" means the declarations line up, not that Doom runs.
# docs/libc_audit.md (SCRUM-72) is the list of what is still owed, per function
# and per call site.
set -uo pipefail

cd /work
mkdir -p build/doom

# -I src puts the freestanding libc shim headers (src/stdio.h, src/string.h,
# src/strings.h, src/inttypes.h, src/errno.h, ...) on the include path. Their
# absence — not anything wrong with the vendored source — is what used to stop
# 77 of these files at their first #include with a fatal "no such file"; 66 of
# them died on <strings.h> alone, which doomtype.h includes on every non-Windows
# build. src/doom comes first so the engine's own headers still shadow.
#
# On -mno-sse, which the kernel build (docker/scripts/build.sh) passes and this
# one deliberately does not:
#
#   The x86_64 SysV ABI returns float and double in xmm0, with no alternative.
#   src/doom/m_config.c's `M_GetFloatVariable()` returns a float, so under
#   -mno-sse GCC rejects it outright — "SSE register return with SSE disabled"
#   — and there is no flag combination that avoids this: -msoft-float and
#   -mfpmath=387 both still hit the ABI. It is the one file of the 79 that
#   cannot compile SSE-free, and the only fix that keeps -mno-sse would be
#   editing vendored source, which SCRUM-63 exists to avoid.
#
#   So Doom needs SSE, and that is a real finding rather than a build-script
#   convenience. IT IS NOT YET SATISFIED AT RUNTIME: src/boot.s sets only
#   CR4.PAE — it never sets CR4.OSFXSR (bit 9) or CR4.OSXMMEXCPT (bit 10), nor
#   clears CR0.EM — so the first SSE instruction any of these objects executes
#   raises #UD today. Enabling SSE, and deciding whether XMM state has to be
#   saved across the syscall and interrupt paths, is a prerequisite for running
#   this code that no ticket owns yet; see docs/libc_audit.md sec5.
#
#   The footprint is larger than the four floats suggest. Only 4 of the 79
#   objects contain float arithmetic at all (g_game, m_config, p_setup,
#   v_video — 18 instructions between them), but allowing SSE also lets GCC
#   inline struct copies and zeroing with movaps/movdqa/pxor across the whole
#   engine (~143 more). Both classes need the same CR4/CR0 bits set.
#
# The kernel's own build keeps -mno-sse and is untouched by any of this: no
# src/*.c uses a float, and the kernel saves no FPU state.
CFLAGS=(-std=gnu99 -ffreestanding -O2 -Wall -Wextra -mno-red-zone -mcmodel=small -mno-mmx -I src/doom -I src)

pass=0
fail=0
failed_files=()

for c in src/doom/*.c; do
  name="$(basename "${c%.c}")"
  o="build/doom/${name}.o"
  if x86_64-elf-gcc -c "$c" -o "$o" "${CFLAGS[@]}" 2>"build/doom/${name}.log"; then
    echo "    OK   $(basename "$c")"
    pass=$((pass + 1))
  else
    echo "    FAIL $(basename "$c")"
    fail=$((fail + 1))
    failed_files+=("$name")
  fi
done

echo ""
echo "doom core compile: $pass passed, $fail failed (out of $((pass + fail)))"
if [[ $fail -gt 0 ]]; then
  echo "failed files (see build/doom/<name>.log for details):"
  printf '  %s\n' "${failed_files[@]}"
  echo ""
  echo "SCRUM-64's acceptance is zero compile errors across src/doom/."
  exit 1
fi

exit 0
