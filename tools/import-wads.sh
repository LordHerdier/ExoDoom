#!/usr/bin/env bash
# Extract a curated set of IWADs from a collection of per-game zips into wads/,
# under the lowercase names src/doom/d_iwad.c searches for, so they can be put
# on the install disk (docker/scripts/build.sh) and `install`ed from the shell.
#
# usage: tools/import-wads.sh <archive.zip> [dest-dir]     (default dest: wads)
#
# <archive.zip> holds one zip per release ("Doom II - Hell on Earth (v1.9).zip"
# ...). Only the releases below are taken: the engine is vanilla Doom, so
# Heretic, Hexen, Strife, Doom 64 and Blasphemer do not belong here.
#
# Budget: the install disk is one ExoFS volume, capped at EXOFS_MAX_BLOCKS
# (128 MiB), and already carries freedoom2.wad (~28 MB) and the packages. What
# is listed below is ~97 MB; adding e.g. freedoom1.wad (23 MB) overflows it.
set -euo pipefail

archive="${1:?usage: $0 <archive.zip> [dest-dir]}"
dest="${2:-wads}"

# <release zip name>|<wad inside it>|<name d_iwad.c looks for>
entries=(
  "Doom (v1.9)|DOOM.WAD|doom.wad"
  "Doom (v1.9) (Demo)|DOOM1.WAD|doom1.wad"
  "Doom II - Hell on Earth (v1.9)|DOOM2.WAD|doom2.wad"
  "Final Doom - The Plutonia Experiment|PLUTONIA.WAD|plutonia.wad"
  "Final Doom - Evilution|TNT.WAD|tnt.wad"
  "Chex Quest|CHEX.WAD|chex.wad"
  "HACX (v1.2)|HACX.WAD|hacx.wad"
)

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$dest"

for e in "${entries[@]}"; do
  IFS='|' read -r release member out <<<"$e"
  if ! unzip -q -o -j "$archive" "$release.zip" -d "$work" 2>/dev/null; then
    echo "skip   $out: '$release.zip' not in $archive"
    continue
  fi
  if ! unzip -q -o -j "$work/$release.zip" "$member" -d "$work/x" 2>/dev/null; then
    echo "skip   $out: $member not in '$release.zip'"
    continue
  fi
  mv "$work/x/$member" "$dest/$out"
  rm -rf "$work/x" "$work/$release.zip"
  printf 'wrote  %s/%s (%s bytes)\n' "$dest" "$out" "$(stat -c %s "$dest/$out")"
done
