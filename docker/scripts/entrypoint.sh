#!/usr/bin/env bash
# Runs build.sh as root (it needs to write anywhere under the bind mount),
# then hands build/ back to whoever owns /work on the host, so `make clean`
# and editors don't need sudo. Runs on failure too (EXIT trap) so a broken
# build doesn't leave a root-owned build/ behind either.
set -uo pipefail

fix_ownership() {
  if [[ -d /work/build ]]; then
    chown -R "$(stat -c '%u:%g' /work)" /work/build
  fi
}
trap fix_ownership EXIT

/usr/local/bin/build.sh "$@"
