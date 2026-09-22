# Shared parallel-job-pool helper, sourced by build.sh / build-doom.sh /
# link-doom.sh. Each of those used to compile its whole source list one
# `x86_64-elf-gcc -c` at a time in a bash `for` loop, which left CI's
# multi-core runner mostly idle -- compile order never affects correctness,
# only the final *link* order does (build_ring3_link_target's own comment in
# build.sh has why that one is order-sensitive). This is the mechanism that
# lets each script run its independent compiles concurrently instead.
#
# Not a standalone script -- `set -e`/`-u` here would apply to whatever
# sources this, which is not this file's call to make.

PARALLEL_JOBS="${PARALLEL_JOBS:-$(nproc)}"

# qcmd <argv...> -- shell-quotes argv into one string safe to hand to `eval`
# (directly, or via run_parallel below), so a job can be built from a plain
# function name plus string args without the caller hand-quoting each one.
qcmd() {
  local out="" a
  for a in "$@"; do
    printf -v a '%q' "$a"
    out+="$a "
  done
  printf '%s' "$out"
}

# run_parallel <cmd...> -- runs each argument (as produced by qcmd, or any
# `eval`-safe string) with at most PARALLEL_JOBS running at once, and returns
# non-zero if any of them failed. Each job runs in a `(...)` subshell of
# *this* shell (not a separate process via bash -c), so it inherits every
# function, array and local variable already in scope at the call site --
# e.g. a job can reference an outer CFLAGS array directly instead of having
# it serialized into the command string.
#
# A shared marker file carries failure out of the background subshells: a
# variable one of them sets never reaches the parent shell, only its exit
# status does, and that status is `wait`'s to report per-job, not summed
# automatically.
run_parallel() {
  local marker running=0
  marker="$(mktemp)"
  rm -f "$marker"
  for cmd in "$@"; do
    ( eval "$cmd" || : > "$marker" ) &
    running=$((running + 1))
    if (( running >= PARALLEL_JOBS )); then
      wait -n
      running=$((running - 1))
    fi
  done
  wait
  if [[ -e "$marker" ]]; then
    rm -f "$marker"
    return 1
  fi
  return 0
}
