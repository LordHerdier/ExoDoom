#!/bin/sh
# Fetch a GNU release tarball, trying each mirror in turn, and verify what
# arrives against a pinned sha256 before the build is allowed to use it.
#
# Why this exists: every docker-* target in the Makefile leads with the
# `docker buildx build` of docker/Dockerfile.build, so a single unreachable
# download host fails the whole of `make docker-build`/`docker-test`/
# `docker-ci` -- with an error ("wget exit 4") that points at the network and
# says nothing about the tree being built. An ftp.gnu.org outage on
# 2026-10-06 did exactly that to a green tree. One host should not be a
# single point of failure for the entire build system.
#
# Why the checksum is not optional: falling back to more hosts means trusting
# more hosts. The pin is what keeps "try another mirror" from also meaning
# "accept whatever some mirror serves". It also catches the failure mirrors
# actually have in practice -- an HTML error page or a truncated transfer
# saved under a .tar.xz name -- at the download rather than as an
# unintelligible tar error minutes later.
#
# Usage: fetch-gnu-tarball.sh <sha256> <gnu-relative-path> [extra-url ...]
#   e.g. fetch-gnu-tarball.sh <sum> binutils/binutils-2.42.tar.xz
# The tarball lands in the working directory under its own basename.

set -eu

if [ "$#" -lt 2 ]; then
    echo "usage: $0 <sha256> <gnu-relative-path> [extra-url ...]" >&2
    exit 2
fi

SHA256="$1"
REL_PATH="$2"
shift 2

OUT="$(basename "$REL_PATH")"

# ftp.gnu.org goes first because it is canonical: the mirrors below can lag it
# by up to a day on a fresh release, so a version bump should still prefer it.
# The rest are long-lived public GNU mirrors run by different operators on
# different networks -- the property that matters is that no two of them go
# down for the same reason.
GNU_MIRRORS="
https://ftp.gnu.org/gnu
https://mirrors.kernel.org/gnu
https://ftp.nluug.nl/pub/gnu
https://mirror.csclub.uwaterloo.ca/gnu
"

urls=""
for base in $GNU_MIRRORS; do
    urls="$urls $base/$REL_PATH"
done
# Caller-supplied absolute URLs go last: they are upstream project hosts with
# their own path layout (sourceware.org for binutils), not /gnu mirrors, so
# they cannot be derived from the relative path above.
urls="$urls $*"

# All progress goes to stderr, including the lines that are not errors: mixing
# the two streams reorders the log, and a mirror's "MISMATCH" then reads as if
# it belonged to the host named on the line after it.
for url in $urls; do
    echo "    fetching $url" >&2
    rm -f "$OUT"
    # Bounded per host: a host that is down must cost seconds, not the build.
    # An ftp.gnu.org whose TCP connect hangs rather than refusing is exactly
    # the case this guards -- without a timeout it stalls here indefinitely
    # and never reaches the mirror that would have worked.
    if ! wget -q --timeout=20 --tries=2 -O "$OUT" "$url"; then
        echo "    ... unreachable, trying next mirror" >&2
        continue
    fi
    if ! echo "$SHA256  $OUT" | sha256sum -c --status -; then
        echo "    ... sha256 MISMATCH (got $(sha256sum "$OUT" | cut -d' ' -f1))" >&2
        echo "    ... refusing this copy, trying next mirror" >&2
        continue
    fi
    echo "    ok: $OUT (sha256 verified)" >&2
    exit 0
done

rm -f "$OUT"
echo "ERROR: could not fetch $REL_PATH from any mirror" >&2
echo "       (tried:$urls )" >&2
exit 1
