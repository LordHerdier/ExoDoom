#!/usr/bin/env bash
# Rehearse the client demo unattended (SCRUM-121): boot the built ISO on a
# zero-filled system disk, headless, and drive tools/demo/demo.script over
# QMP. Screenshots and the serial log land in build/demo/; the run passes
# only if every `expect` line in the script holds.
#
# Needs qemu-system-x86_64 and python3 on the host (like `make run`), and a
# finished build. Run it as `make demo-rehearse`, which builds first and
# passes DEMO_QEMU_DEVICES: the Makefile's own USB device list (the install stick),
# so the rehearsal boots the same machine `make run-blank` does.
set -euo pipefail
cd "$(dirname "$0")/../.."
: "${DEMO_QEMU_DEVICES:?run this as 'make demo-rehearse'}"

out=build/demo
rm -rf "$out"
mkdir -p "$out"

# Same blank disk as `make run-blank`: the size of the real one, all zeros,
# so the shell offers to format it.
rm -f build/exodoom_blank.img
truncate -s "$(stat -c %s build/exodoom_disk.img)" build/exodoom_blank.img

qmp_port=${QMP_PORT:-4447}
# shellcheck disable=SC2086  # DEMO_QEMU_DEVICES is a list of QEMU arguments
qemu-system-x86_64 -m 256M -cdrom build/exodoom.iso \
  -drive file=build/exodoom_blank.img,format=raw,if=ide,index=0 \
  $DEMO_QEMU_DEVICES \
  -device intel-hda -device hda-output,audiodev=snd0 -audiodev none,id=snd0 \
  -no-reboot -display none -monitor none \
  -serial "file:$out/serial.log" \
  -qmp "tcp:127.0.0.1:$qmp_port,server=on,wait=off" &
qemu_pid=$!
trap 'kill $qemu_pid 2>/dev/null || true' EXIT

python3 tools/demo/qmp_drive.py "$qmp_port" tools/demo/demo.script "$out" \
  "$out/serial.log"
