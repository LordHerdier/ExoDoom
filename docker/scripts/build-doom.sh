#!/usr/bin/env bash
# build-doom.sh — best-effort compile pass over the vendored doomgeneric
# core engine (src/doom/*.c) with the same freestanding cross-compiler used
# for the kernel.
#
# This is NOT part of docker-build/docker-ci: most of these files still call
# libc functions the kernel's freestanding libc shim hasn't implemented yet
# (see docs/syscall_spec.md §2 for the audit). Run it to see current
# per-file pass/fail status while the libc shim fills in.
set -uo pipefail

cd /work
mkdir -p build/doom

CFLAGS=(-std=gnu99 -ffreestanding -O2 -Wall -Wextra -mno-red-zone -mcmodel=small -mno-sse -mno-sse2 -mno-mmx -I src/doom)

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
fi

exit 0
