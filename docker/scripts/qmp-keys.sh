#!/usr/bin/env bash
#
# qmp-keys.sh — type into the two USB keyboards of a test boot (SCRUM-198).
#
# Usage: qmp-keys.sh [host] [port]   (default 127.0.0.1 4444)
#
# docker-test/docker-ci start QEMU with `-qmp tcp:<host>:<port>,server=on,
# wait=off` and run this in the background. It sends "a" to the console
# keyboard A is bound to (`display=vga2`) and "b" to keyboard B's (vga3),
# every ~200 ms, until QEMU goes away. tests/kernel/test_usb_k.c checks that
# each keyboard saw only its own key (docs/drivers/usb.md section 12).
#
# Pure bash (/dev/tcp): the exodoom-qemu image has no socat or python, and
# this is not worth growing it for. Everything QEMU answers is discarded.

set -u

host=${1:-127.0.0.1}
port=${2:-4444}

# QEMU may not be listening yet. A failed `exec` redirection would end a
# non-interactive shell, so probe in a subshell first.
connected=0
for _ in $(seq 1 300); do
    if (exec 3<>"/dev/tcp/$host/$port") 2>/dev/null; then
        exec 3<>"/dev/tcp/$host/$port" && connected=1 && break
    fi
    sleep 0.1
done
[ "$connected" = 1 ] || exit 0

# Drain replies so QEMU never blocks on a full socket.
cat <&3 >/dev/null &

send() {
    printf '%s\n' "$1" >&3 2>/dev/null
}

key() {   # key <console id> <qcode> <true|false>
    send "{\"execute\":\"input-send-event\",\"arguments\":{\"device\":\"$1\",\"events\":[{\"type\":\"key\",\"data\":{\"down\":$3,\"key\":{\"type\":\"qcode\",\"data\":\"$2\"}}}]}}"
}

send '{"execute":"qmp_capabilities"}' || exit 0

while key vga2 a true; do
    sleep 0.05
    key vga2 a false || break
    key vga3 b true || break
    sleep 0.05
    key vga3 b false || break
    sleep 0.1
done
exit 0
