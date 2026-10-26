#!/usr/bin/env bash
# Render every docs/diagrams/*.d2 to SVG (primary) and PNG (2x, for slides).
#
#   docs/diagrams/render.sh            render all
#   docs/diagrams/render.sh 03-*.d2    render some
#
# NixOS: d2 and librsvg are pulled in on demand, nothing is installed. Any
# other host with d2 >= 0.6 and rsvg-convert on PATH works the same way.
# The ELK layout engine is used because it is bundled with every d2 build and
# does not depend on TALA, which d2 only ships with a watermark.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

if ! command -v d2 >/dev/null || ! command -v rsvg-convert >/dev/null; then
    if command -v nix >/dev/null; then
        exec nix shell nixpkgs#d2 nixpkgs#librsvg nixpkgs#source-sans-pro nixpkgs#source-code-pro \
            --command "$0" "$@"
    fi
    echo "render.sh: need d2 and rsvg-convert (or nix)" >&2
    exit 1
fi

if [ "$#" -gt 0 ]; then srcs=("$@"); else srcs=([0-9][0-9]-*.d2); fi

for src in "${srcs[@]}"; do
    base="${src%.d2}"
    echo "== $base"
    d2 --layout=elk --pad=40 --theme=0 "$src" "$base.svg"
    # d2 embeds its fonts as base64 @font-face rules, which rsvg ignores, so
    # point the PNG copy at the same family installed on the system. The SVG
    # itself is left untouched and renders with its embedded fonts.
    tmp="$(mktemp --suffix=.svg)"
    sed -E 's/"?d2-[0-9]+-font-regular"?/"Source Sans Pro"/g;
            s/"?d2-[0-9]+-font-bold"?/"Source Sans Pro"; font-weight: 700/g' \
        "$base.svg" > "$tmp"
    rsvg-convert --zoom=2 --background-color=white "$tmp" -o "$base.png"
    rm -f "$tmp"
done
