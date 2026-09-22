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

# -- Sound is a *configured* no-op, and this is the gate that keeps it one ----
#
# SCRUM-82 asked for I_StartSound/I_StopSound/I_UpdateSound to be stubbed to
# no-ops, so that the Audio epic (SCRUM-98..101) sits off the critical path to
# a running game rather than in front of it. The vendored tree already answers
# that without a single edit, which is why this step is a check and not a stub:
#
#   src/doom/doomfeatures.h leaves FEATURE_SOUND undefined (line 36 has the
#   #undef itself commented out), so in src/doom/i_sound.c the compiled-in
#   module table collapses to `sound_modules[] = { NULL }`, InitSfxModule()
#   finds nothing to initialise, InitMusicModule() has an empty body, and both
#   `sound_module` and `music_module` stay NULL for the life of the process.
#   Every I_* entry point in that file already null-checks those two pointers
#   and returns a benign value -- 0 from I_StartSound and I_GetSfxLumpNum,
#   false from I_SoundIsPlaying, nothing at all from I_StopSound and
#   I_UpdateSound. s_sound.c's callers are fine with that: the handle it
#   stores is never played, and I_SoundIsPlaying(0) answering false simply
#   retires the channel.
#
# This is Option A in docs/syscall_spec.md sec6, chosen there on purpose. So
# the ticket's acceptance ("no crashes from sound function calls; game is
# silent but functional") holds today, and what was actually missing was
# anything that would notice if it stopped holding.
#
# What would stop it holding is FEATURE_SOUND becoming defined. Note which
# half of that this gate is actually for, because the obvious half is already
# covered: a plain -DFEATURE_SOUND fails in the compile loop above all by
# itself, since i_sound.c's `#include <SDL_mixer.h>` sits under the same guard
# and there is no SDL_mixer.h in this image (verified -- "fatal error:
# SDL_mixer.h: No such file or directory"). That path is loud and needs no
# help from us.
#
# The quiet half is FEATURE_SOUND defined with that include bypassed --
# -D__DJGPP__ alongside it, which the guard explicitly excludes, or an
# SDL_mixer.h arriving on the include path from some future vendored shim.
# Then i_sound.c compiles perfectly well (verified), and the two module
# symbols it now references -- DG_sound_module/DG_music_module, declared
# extern in i_sound.h and defined in none of the 79 files -- are left
# undefined. A compile-only pass has nothing to say about that, so the first
# sign would be SCRUM-66's link: the one step in the project with three epics
# landing on it at once, and the one that least needs a fourth,
# self-inflicted undefined-symbol hunt.
#
# So: assert the shape in the object file rather than in the source, since the
# object file is what the linker will actually see.
echo ""
echo "sound configuration gate (SCRUM-82)"

sound_o="build/doom/i_sound.o"

# 1. No sound-module symbol may be left undefined. This is the check that
#    fires if FEATURE_SOUND comes back: the #ifdef'd references in
#    sound_modules[] and InitMusicModule() are the only two in the tree.
#    Mix_/SDL_ are in the same list because i_sound.c's <SDL_mixer.h> include
#    sits under the same guard, and an SDL symbol reaching the link is the
#    same mistake wearing a different hat.
#
#    Run nm on its own first, so a tooling failure is distinguishable from
#    "no matches". This script sets `pipefail`, so with nm inside the
#    pipeline any nm error fails the whole pipeline -- and the `|| true`
#    that has to be there for grep (which exits 1 in the ordinary case of
#    finding nothing) would swallow it, leaving sound_undef empty and this
#    gate reporting OK for an object it never managed to read. The
#    defined-symbol check below never had the problem, because it tests
#    nm's own exit status directly. Review catch on PR #100.
if ! sound_nm="$(x86_64-elf-nm -u "$sound_o")"; then
  echo "    ERROR: x86_64-elf-nm failed to read $sound_o."
  echo "           This gate cannot speak for an object it could not read."
  exit 1
fi

sound_undef="$(printf '%s\n' "$sound_nm" \
  | grep -oE 'DG_sound_module|DG_music_module|Mix_[A-Za-z0-9_]*|SDL_[A-Za-z0-9_]*' \
  | sort -u || true)"

if [[ -n "$sound_undef" ]]; then
  echo "    ERROR: $sound_o references sound-module symbols that nothing defines:"
  printf '             %s\n' $sound_undef
  echo ""
  echo "    FEATURE_SOUND is supposed to stay undefined (src/doom/doomfeatures.h),"
  echo "    which is what makes I_StartSound/I_StopSound/I_UpdateSound no-ops and"
  echo "    keeps the Audio epic off the critical path -- docs/syscall_spec.md sec6"
  echo "    Option A, SCRUM-82. With it defined these symbols have no definition"
  echo "    anywhere in src/doom/, and SCRUM-66's link is where you would find out."
  exit 1
fi

# 2. The three entry points the ticket names must still be *defined* here.
#    Symmetric to the check above, guarding the opposite mistake: deleting them
#    or #ifdef-ing them away would also compile silently and produce an
#    undefined symbol at link from the other direction -- s_sound.c calls all
#    three unconditionally (s_sound.c:475, :165, :513).
for sym in I_StartSound I_StopSound I_UpdateSound; do
  if ! x86_64-elf-nm -g --defined-only "$sound_o" | grep -qE " T $sym\$"; then
    echo "    ERROR: $sound_o no longer defines $sym."
    echo "           s_sound.c calls it unconditionally; the link needs it."
    exit 1
  fi
done

echo "    OK   FEATURE_SOUND off; I_StartSound/I_StopSound/I_UpdateSound are defined no-ops"

exit 0
