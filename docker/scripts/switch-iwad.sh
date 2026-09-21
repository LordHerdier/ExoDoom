#!/usr/bin/env bash
set -euo pipefail

# switch-iwad.sh -- flip the IWAD ExoDoom links against (SCRUM-90).
#
# The IWAD name is a single hardcoded constant, deliberately spelled in
# three places that docker/scripts/build.sh's "IWAD filename is spelled in
# three places" check asserts agree: build.sh's own WAD_PATH (which
# DOOM_IWAD already selects), src/grub.cfg's `module2` line, and
# src/doomgeneric_exo.c's DOOM_IWAD_BLOB_NAME. There is no runtime IWAD
# selector -- doomgeneric_exo.c's own comment says that's SCRUM-75/
# SCRUM-190's job -- so testing a different IWAD means hand-flipping the
# latter two spellings and rebuilding. This script is that hand-flip,
# automated so it can't drift into a three-way mismatch (build.sh already
# catches that at build time, but there's no reason to ever hit it).
#
# Usage:
#   docker/scripts/switch-iwad.sh freedoom2   # back to the shipped default
#   docker/scripts/switch-iwad.sh doom2       # the original commercial IWAD
#
# Wired into the Makefile as `make switch-iwad IWAD=doom2`.

cd "$(git rev-parse --show-toplevel)"

usage() {
  echo "usage: $0 {freedoom2|doom2}" >&2
  exit 1
}

[[ $# -eq 1 ]] || usage

case "$1" in
  freedoom2) WAD_FILE="freedoom2.wad" ;;
  doom2)     WAD_FILE="doom2.wad" ;;
  *)         usage ;;
esac

GRUB_CFG="src/grub.cfg"
DOOMGENERIC_EXO="src/doomgeneric_exo.c"

current_blob="$(sed -n 's/^#define DOOM_IWAD_BLOB_NAME "\(.*\)"$/\1/p' "$DOOMGENERIC_EXO")"
current_grub="$(sed -n 's@.*module2 /boot/\([^ ]*\).*@\1@p' "$GRUB_CFG" | head -1)"

if [[ "$current_blob" == "$WAD_FILE" && "$current_grub" == "$WAD_FILE" ]]; then
  echo "Already wired to $WAD_FILE ($GRUB_CFG and $DOOMGENERIC_EXO both agree) -- nothing to do."
else
  sed -i "s|^#define DOOM_IWAD_BLOB_NAME \".*\"\$|#define DOOM_IWAD_BLOB_NAME \"${WAD_FILE}\"|" "$DOOMGENERIC_EXO"
  sed -i "s|module2 /boot/[^ ]* [^ ]*|module2 /boot/${WAD_FILE} ${WAD_FILE}|" "$GRUB_CFG"
  echo "Wired $GRUB_CFG and $DOOMGENERIC_EXO to $WAD_FILE."
fi

# Not fatal -- build.sh's own check is the real gate, and DOOM_IWAD=freedoom2
# fetches its file automatically. This is just a heads-up before a build that
# would otherwise fail several minutes in.
STAGE_PATH="build/${WAD_FILE}"
if [[ "$1" == "doom2" && ! -f "$STAGE_PATH" ]]; then
  echo ""
  echo "NOTE: $STAGE_PATH is not staged yet. DOOM_IWAD=doom2 does not"
  echo "      auto-fetch it (commercial IWAD, unlike Freedoom) -- place it"
  echo "      there by hand first, e.g.:"
  echo "        curl -fL -o $STAGE_PATH \\"
  echo "          https://archive.org/download/DOOM2IWADFILE/DOOM2.WAD"
fi

echo ""
echo "Next: make docker-build DOOM_IWAD=$1   (or docker-run / docker-test)"
echo ""
echo "These are tracked source files -- 'git diff -- $GRUB_CFG $DOOMGENERIC_EXO'"
echo "before committing, and switch back to freedoom2 before merging."
