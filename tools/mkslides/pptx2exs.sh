#!/usr/bin/env bash
# pptx2exs.sh -- turn a presentation into an ExoDoom slide deck (.exs).
#
#   tools/mkslides/pptx2exs.sh <deck.pptx|deck.pdf> <out.exs> [width]
#
# Runs on the build host, not in Docker: the rendering needs LibreOffice
# (only for .pptx input) and poppler's pdftoppm, neither of which belongs in
# the cross-toolchain image. The steps are
#
#   .pptx --soffice--> .pdf --pdftoppm--> one .ppm per slide --mkslides--> .exs
#
# `width` (default 1024, the QEMU framebuffer's width) is the render width
# in pixels; the height follows the page's aspect ratio, so a 16:9 deck comes
# out 1024x576 and the viewer letterboxes it on a 1024x768 screen with no
# scaling at all.
#
# Fonts: LibreOffice renders with what is installed on this machine. A deck
# set in fonts it does not have (Calibri, Consolas, ...) is re-flowed with
# substitutes and text can overlap. If that happens, export a PDF from
# PowerPoint itself and pass the .pdf instead -- the PDF carries its fonts.
#
# Put the result in slides/ at the repo root; docker/scripts/build.sh writes
# every slides/*.exs onto the program volume, and `slides` in the shell
# shows it.
set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
  echo "usage: $0 <deck.pptx|deck.pdf> <out.exs> [width]" >&2
  exit 1
fi

in="$1"
out="$2"
width="${3:-1024}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

for tool in pdftoppm cc; do
  command -v "$tool" >/dev/null || { echo "$0: $tool not found" >&2; exit 1; }
done

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

case "${in,,}" in
  *.pdf)
    pdf="$in"
    ;;
  *.pptx|*.ppt|*.odp)
    soffice="$(command -v soffice || command -v libreoffice || true)"
    [[ -n "$soffice" ]] || { echo "$0: LibreOffice (soffice) not found" >&2; exit 1; }
    # A private profile, so a LibreOffice already open on the desktop does not
    # swallow the headless conversion and exit without producing anything.
    "$soffice" -env:UserInstallation="file://$work/lo-profile" --headless \
      --convert-to pdf --outdir "$work" "$in" >/dev/null 2>&1
    pdf="$work/$(basename "${in%.*}").pdf"
    [[ -s "$pdf" ]] || { echo "$0: LibreOffice produced no PDF" >&2; exit 1; }
    ;;
  *)
    echo "$0: $in: expected .pptx, .ppt, .odp or .pdf" >&2
    exit 1
    ;;
esac

pdftoppm -scale-to-x "$width" -scale-to-y -1 "$pdf" "$work/slide"

cc -std=c99 -O2 -Wall -Wextra -Werror "$here/main.c" -o "$work/mkslides"

# pdftoppm zero-pads page numbers to a fixed width, so a glob sorts in order.
"$work/mkslides" "$out" "$work"/slide-*.ppm
"$work/mkslides" --check "$out"
