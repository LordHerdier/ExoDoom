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

# shellcheck source=parallel.sh
source "$(dirname "${BASH_SOURCE[0]}")/parallel.sh"

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
#   convenience. IT IS SATISFIED AT RUNTIME as of SCRUM-177: src/boot.s's
#   _start64 clears CR0.EM/CR0.TS, sets CR0.MP/CR0.NE and
#   CR4.OSFXSR/CR4.OSXMMEXCPT, and loads MXCSR = 0x1F80, before kernel_main
#   runs. tests/kernel/test_sse_k.c proves an SSE instruction executes and is
#   correct, in ring 0 and from a ring-3 LibOS.
#
#   The footprint is larger than the four floats suggest. Only 4 of the 79
#   objects contain float arithmetic at all (g_game, m_config, p_setup,
#   v_video — 18 instructions between them), but allowing SSE also lets GCC
#   inline struct copies and zeroing with movaps/movdqa/pxor across the whole
#   engine (~143 more). Both classes need the same CR4/CR0 bits set.
#
# The kernel's own build keeps -mno-sse and is untouched by any of this: no
# src/*.c uses a float, and the kernel saves no FPU state. That second half is
# now load-bearing rather than incidental -- it is exactly why syscall_entry.s
# and isr.s need no fxsave (docs/syscall_spec.md sec3.4a). Dropping -mno-sse
# from the *kernel's* CFLAGS, as opposed to this file's, would break that.
CFLAGS=(-std=gnu99 -ffreestanding -O2 -Wall -Wextra -mno-red-zone -mcmodel=small -mno-mmx -I src/doom -I src)

# Each file's pass/fail is independent, so the 79 compiles run in parallel
# (docker/scripts/parallel.sh) instead of one at a time; a job can't hand a
# pass/fail count back to this shell directly (it runs in a forked
# subshell), so each one appends its own "OK <name>"/"FAIL <name>" line to a
# shared results file instead, and this shell tallies it after run_parallel
# returns.
results="$(mktemp)"

_compile_doom_gate_one() {
  local c="$1" o="$2" name="$3"
  if x86_64-elf-gcc -c "$c" -o "$o" "${CFLAGS[@]}" 2>"build/doom/${name}.log"; then
    echo "OK $name" >> "$results"
  else
    echo "FAIL $name" >> "$results"
  fi
}

cmds=()
for c in src/doom/*.c; do
  name="$(basename "${c%.c}")"
  o="build/doom/${name}.o"
  cmds+=("$(qcmd _compile_doom_gate_one "$c" "$o" "$name")")
done
run_parallel "${cmds[@]}"

pass=0
fail=0
failed_files=()
while read -r status name; do
  [[ -z "$status" ]] && continue
  if [[ "$status" == OK ]]; then
    echo "    OK   ${name}.c"
    pass=$((pass + 1))
  else
    echo "    FAIL ${name}.c"
    fail=$((fail + 1))
    failed_files+=("$name")
  fi
done < <(sort -k2 "$results")
rm -f "$results"

echo ""
echo "doom core compile: $pass passed, $fail failed (out of $((pass + fail)))"
if [[ $fail -gt 0 ]]; then
  echo "failed files (see build/doom/<name>.log for details):"
  printf '  %s\n' "${failed_files[@]}"
  echo ""
  echo "SCRUM-64's acceptance is zero compile errors across src/doom/."
  exit 1
fi

# -- Sound is ON, backed by the PC speaker, and this gate keeps it that way --
#
# History: SCRUM-82 shipped with FEATURE_SOUND undefined, which made
# I_StartSound/I_StopSound/I_UpdateSound configured no-ops (sound_modules[]
# collapsed to { NULL }), and this gate existed to keep them no-ops. SCRUM-101
# turns sound on: src/doom/doomfeatures.h now #defines FEATURE_SOUND, and
# src/doom_sound.c -- outside the vendored tree, linked into the libos_doom
# target and into docker/scripts/link-doom.sh's gate -- defines the
# DG_sound_module/DG_music_module that i_sound.c then references.
#
# The ways this can silently regress are therefore the mirror image of
# SCRUM-82's, and each is a check on the object file (what the linker sees),
# not the source:
#
#   1. i_sound.o must reference DG_sound_module and DG_music_module. If it
#      does not, FEATURE_SOUND has gone off again: everything still compiles
#      and links, and Doom is silent with nobody told. Defining it with
#      -DFEATURE_SOUND instead would be loud (i_sound.c's <SDL_mixer.h>
#      guard sits above its include of doomfeatures.h and would fire), which
#      is exactly why it is defined in the header.
#   2. i_sound.o must not reference any Mix_*/SDL_* symbol: an SDL call
#      reaching the link has no definition anywhere in this project.
#   3. I_StartSound/I_StopSound/I_UpdateSound must still be *defined* here --
#      s_sound.c calls all three unconditionally (s_sound.c:475, :165, :513).
#
# Whether DG_sound_module/DG_music_module actually resolve is link-doom.sh's
# job, which compiles src/doom_sound.c next to these objects.
echo ""
echo "sound configuration gate (SCRUM-101; was SCRUM-82)"

sound_o="build/doom/i_sound.o"

# Run nm on its own first, so a tooling failure is distinguishable from "no
# matches" -- with nm inside a pipefail pipeline, the `|| true` grep needs
# would swallow an nm error and leave the checks below reading nothing.
# Review catch on PR #100.
if ! sound_nm="$(x86_64-elf-nm -u "$sound_o")"; then
  echo "    ERROR: x86_64-elf-nm failed to read $sound_o."
  echo "           This gate cannot speak for an object it could not read."
  exit 1
fi

for sym in DG_sound_module DG_music_module; do
  if ! printf '%s\n' "$sound_nm" | grep -qE " U $sym\$"; then
    echo "    ERROR: $sound_o no longer references $sym."
    echo ""
    echo "    FEATURE_SOUND has gone off (src/doom/doomfeatures.h). Doom would"
    echo "    build, link and run -- silently. SCRUM-101 turned it on; the PC"
    echo "    speaker module is src/doom_sound.c."
    exit 1
  fi
done

sdl_undef="$(printf '%s\n' "$sound_nm" \
  | grep -oE 'Mix_[A-Za-z0-9_]*|SDL_[A-Za-z0-9_]*' | sort -u || true)"
if [[ -n "$sdl_undef" ]]; then
  echo "    ERROR: $sound_o references SDL/SDL_mixer symbols nothing defines:"
  printf '             %s\n' $sdl_undef
  echo "           i_sound.c's <SDL_mixer.h> include must stay unreachable;"
  echo "           see the comment on FEATURE_SOUND in src/doom/doomfeatures.h."
  exit 1
fi

for sym in I_StartSound I_StopSound I_UpdateSound; do
  if ! x86_64-elf-nm -g --defined-only "$sound_o" | grep -qE " T $sym\$"; then
    echo "    ERROR: $sound_o no longer defines $sym."
    echo "           s_sound.c calls it unconditionally; the link needs it."
    exit 1
  fi
done

echo "    OK   FEATURE_SOUND on; i_sound.o uses DG_sound_module/DG_music_module, no SDL"

exit 0
