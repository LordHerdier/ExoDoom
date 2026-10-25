.PHONY: docker-build docker-run docker-run-kernel docker-run-blank run-blank docker-test docker-build-doom docker-link-doom clean

DEBUG ?= 0
# AUDIO_PROFILE=1 compiles in the MUSPROF/HDAPROF serial timing reports
# (docs/drivers/hda.md 12.12).
AUDIO_PROFILE ?= 0

# Extra flags for the exodoom-build image's buildx invocation, e.g.
# "--cache-from type=gha --cache-to type=gha,mode=max" in CI. Empty locally.
BUILDX_CACHE_ARGS ?=

# USB topology shared by every QEMU line below (SCRUM-245; the reasoning and
# measurements are in docs/drivers/usb.md). Three UHCI controllers = 6 root
# ports. Every device names its port= -- without it QEMU slips an automatic
# usb-hub onto the last free port, and the kernel has no hub driver. Each
# usb-kbd is bound to its own secondary-vga console with display=, or QEMU
# would send every host keystroke to it instead of the PS/2 keyboard.
QEMU_USB_ARGS = \
  -device ich9-usb-uhci1,id=uhci1 \
  -device ich9-usb-uhci2,id=uhci2 \
  -device ich9-usb-uhci3,id=uhci3 \
  -device secondary-vga,id=vga2 \
  -device secondary-vga,id=vga3 \
  -device usb-kbd,bus=uhci1.0,port=1,display=vga2 \
  -device usb-kbd,bus=uhci1.0,port=2,display=vga3 \
  -drive if=none,id=usbstick,file=build/usb.img,format=raw \
  -device usb-storage,bus=uhci3.0,port=1,drive=usbstick

# Second IDE drive (SCRUM-244): the primary slave, `index=1`, which the kernel
# registers as block device 1 -- the stand-in for an install stick until USB
# mass storage (SCRUM-247) exists. The system disk is pinned to `index=0` on
# every line so the two can never swap. docker/scripts/build.sh rewrites the
# image on every build as an ExoFS volume holding the IWAD and every .exo, the
# source the shell's `install` copies from (SCRUM-233). A test boot reformats
# it (test_multidisk_k.c, test_shell_install_k.c), so a hand-rolled QEMU line
# run after `make docker-test` without a rebuild finds it empty.
# -cdrom keeps IDE index 2, the secondary master, which the kernel skips.
QEMU_DISK2_ARGS = -drive file=build/exodoom_disk2.img,format=raw,if=ide,index=1

# usb-mouse has no display= property, so in a GUI display it would take the
# host mouse away from PS/2. Test boots only until SCRUM-246 drives it.
QEMU_USB_TEST_ARGS = $(QEMU_USB_ARGS) \
  -device usb-mouse,bus=uhci2.0,port=1

# Test boots also open QMP so docker/scripts/qmp-keys.sh can type into the
# two USB keyboards separately -- "a" on vga2's, "b" on vga3's -- for
# tests/kernel/test_usb_k.c's acceptance check (SCRUM-198). The script is
# started in the background just before QEMU and retries until QMP listens.
QMP_TEST_PORT = 4444
QEMU_QMP_TEST_ARGS = -qmp tcp:127.0.0.1:$(QMP_TEST_PORT),server=on,wait=off
QMP_KEYS = bash /work/docker/scripts/qmp-keys.sh 127.0.0.1 $(QMP_TEST_PORT) &

docker-build:
	docker buildx build $(BUILDX_CACHE_ARGS) --load -t exodoom-build -f docker/Dockerfile.build docker
	docker run --rm -e DEBUG=$(DEBUG) -e AUDIO_PROFILE=$(AUDIO_PROFILE) -v "$(PWD):/work" exodoom-build

docker-run: docker-build
	docker build -t exodoom-qemu -f docker/Dockerfile.qemu docker
	docker run --rm -it -v "$(PWD):/work" exodoom-qemu \
	  'qemu-system-x86_64 -cdrom build/exodoom.iso -drive file=build/exodoom_disk.img,format=raw,if=ide,index=0 $(QEMU_DISK2_ARGS) -m 256M -device intel-hda -device hda-output,audiodev=snd0 -audiodev none,id=snd0 $(QEMU_USB_ARGS) -no-reboot -display curses -serial mon:stdio'

# Dev-loop boot target: skips the GRUB menu, not GRUB itself.
#
# `qemu -kernel` cannot be used here: it only loads Multiboot 1 or PVH
# kernels, and ExoDoom is Multiboot 2 (the kernel needs the MB2 memory map
# and framebuffer tags), so it fails with "Error loading uncompressed kernel
# without PVH ELF Note" (issue #84). We boot the ISO instead; src/grub.cfg
# sets timeout=0 so GRUB goes straight to the only entry with no menu.
#
# -drive ...build/exodoom_disk.img: the ExoFS volume holding freedoom2.wad
# (SCRUM-190, docker/scripts/build.sh step [1b/7]), replacing the old GRUB
# multiboot2 module -- see src/syscall_launch.c.
docker-run-kernel: docker-build
	docker build -t exodoom-qemu -f docker/Dockerfile.qemu docker
	docker run --rm -it -v "$(PWD):/work" exodoom-qemu \
	  'qemu-system-x86_64 -cdrom build/exodoom.iso -drive file=build/exodoom_disk.img,format=raw,if=ide,index=0 $(QEMU_DISK2_ARGS) -m 256M -device intel-hda -device hda-output,audiodev=snd0 -audiodev none,id=snd0 $(QEMU_USB_ARGS) -no-reboot -display curses -serial mon:stdio'

# First-boot demo (SCRUM-234): same as docker-run-kernel, but the disk is an
# all-zero image the size of the real one, so the shell finds no ExoFS
# superblock and offers "no system volume -- format? [y/N]". Regenerated
# (re-zeroed) on every run, so each boot starts from a new install;
# build/exodoom_disk.img is left alone.
docker-run-blank: docker-build
	rm -f build/exodoom_blank.img
	truncate -s "$$(stat -c %s build/exodoom_disk.img)" build/exodoom_blank.img
	docker build -t exodoom-qemu -f docker/Dockerfile.qemu docker
	docker run --rm -it -v "$(PWD):/work" exodoom-qemu \
	  'qemu-system-x86_64 -cdrom build/exodoom.iso -drive file=build/exodoom_blank.img,format=raw,if=ide,index=0 $(QEMU_DISK2_ARGS) -m 256M -device intel-hda -device hda-output,audiodev=snd0 -audiodev none,id=snd0 $(QEMU_USB_ARGS) -no-reboot -display curses -serial mon:stdio'

# build/exodoom_disk.img (written by docker/scripts/build.sh step [2e2/7],
# SCRUM-190) replaces build/ata_scratch.img here too: its low LBAs keep that
# scratch image's exact layout (see src/doom_wad_disk.h's own comment for
# the reserved ranges), so test_ata_k.c/test_syscall_disk_k.c/test_exofs_k.c
# need no changes, and the same image now also lets the WAD-dependent KUnit
# suites (test_doom_sound_pcm_k.c, test_hda_pcm_k.c, test_doom_dmx_k.c) read
# the real IWAD through doom_wad_disk_get() instead of the retired
# mmap_find_module()/GRUB-module lookup. No dd step needed: build.sh's
# image builder already produces the whole file, scratch region included.
docker-test:
	docker buildx build $(BUILDX_CACHE_ARGS) --load -t exodoom-build -f docker/Dockerfile.build docker
	docker run --rm -e DEBUG=$(DEBUG) -e AUDIO_PROFILE=$(AUDIO_PROFILE) -e TESTING=1 -v "$(PWD):/work" exodoom-build
	docker build -t exodoom-qemu -f docker/Dockerfile.qemu docker
	docker run --rm --entrypoint bash -v "$(PWD):/work" exodoom-qemu -lc '\
	  set -eu; \
	  rm -f /work/serial.log; \
	  $(QMP_KEYS) \
	  timeout 180 qemu-system-x86_64 \
	  -cdrom build/exodoom.iso \
	  -drive file=/work/build/exodoom_disk.img,format=raw,if=ide,index=0 \
	  $(QEMU_DISK2_ARGS) \
	  -device intel-hda \
	  -device hda-output,audiodev=snd0 \
	  -audiodev none,id=snd0 \
	  $(QEMU_USB_TEST_ARGS) \
	  $(QEMU_QMP_TEST_ARGS) \
	  -m 256M \
	  -no-reboot \
	  -display none \
	  -monitor none \
	  -serial file:/work/serial.log \
	  -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
	  || true; \
	  cat /work/serial.log || true; \
	  '

docker-ci:
	docker buildx build $(BUILDX_CACHE_ARGS) --load -t exodoom-build -f docker/Dockerfile.build docker
	docker run --rm -e DEBUG=$(DEBUG) -e AUDIO_PROFILE=$(AUDIO_PROFILE) -e TESTING=1 -v "$(PWD):/work" exodoom-build
	docker build -t exodoom-qemu -f docker/Dockerfile.qemu docker
	docker run --rm --entrypoint bash -v "$(PWD):/work" exodoom-qemu -lc '\
	  set -eu; \
	  rm -f /work/serial.log; \
	  $(QMP_KEYS) \
	  timeout 180 qemu-system-x86_64 \
	  -cdrom build/exodoom.iso \
	  -drive file=/work/build/exodoom_disk.img,format=raw,if=ide,index=0 \
	  $(QEMU_DISK2_ARGS) \
	  -device intel-hda \
	  -device hda-output,audiodev=snd0 \
	  -audiodev none,id=snd0 \
	  $(QEMU_USB_TEST_ARGS) \
	  $(QEMU_QMP_TEST_ARGS) \
	  -m 256M \
	  -no-reboot \
	  -display none \
	  -monitor none \
	  -serial file:/work/serial.log \
	  -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
	  || true; \
	  cat /work/serial.log || true; \
	  '

# Best-effort compile pass over vendored src/doom/*.c (SCRUM-63/SCRUM-72).
# Not part of docker-build/docker-ci -- many files still need libc gaps filled.
docker-build-doom:
	docker buildx build $(BUILDX_CACHE_ARGS) --load -t exodoom-build -f docker/Dockerfile.build docker
	docker run --rm --entrypoint bash -v "$(PWD):/work" exodoom-build /work/docker/scripts/build-doom.sh

# SCRUM-65's acceptance gate: compile the libc shim the way a ring-3 LibOS
# target would, put it next to the doom objects, and fail if any libc symbol
# is still undefined. docker-build-doom above proves src/doom/ COMPILES; this
# proves the shim under it is complete. The four DG_* platform callbacks are
# expected to remain and are allowlisted by ticket inside the script.
docker-link-doom:
	docker buildx build $(BUILDX_CACHE_ARGS) --load -t exodoom-build -f docker/Dockerfile.build docker
	docker run --rm --entrypoint bash -v "$(PWD):/work" exodoom-build /work/docker/scripts/link-doom.sh

docker-run-debug: docker-build
	docker build -t exodoom-qemu -f docker/Dockerfile.qemu docker
	docker run --rm -it -p 1234:1234 -v "$(PWD):/work" exodoom-qemu \
	'qemu-system-x86_64 -cdrom build/exodoom.iso -drive file=build/exodoom_disk.img,format=raw,if=ide,index=0 $(QEMU_DISK2_ARGS) -m 256M -device intel-hda -device hda-output,audiodev=snd0 -audiodev none,id=snd0 $(QEMU_USB_ARGS) -no-reboot -serial mon:stdio -s -S'

# Boots on the HOST's qemu rather than in the container, which is the only
# place a speaker exists -- swap `-audiodev none,id=snd0` below for
# `-audiodev pipewire,id=snd0` (or pa/alsa/coreaudio) to actually hear SCRUM-210's
# test tone. Not `sdl`: it drains the HDA ~8% fast and unevenly, which makes the
# music's tempo wrong (docs/drivers/hda.md 12.12). Everything under docker-* keeps the null backend: it still runs
# the full BDL DMA and completion-interrupt path, which is what the KUnit
# suite asserts on, and headless Docker has no audio backend to hand.
run: docker-build
	qemu-system-x86_64 -m 256M -cdrom build/exodoom.iso -drive file=build/exodoom_disk.img,format=raw,if=ide,index=0 $(QEMU_DISK2_ARGS) -device intel-hda -device hda-output,audiodev=snd0 -audiodev none,id=snd0 $(QEMU_USB_ARGS) -no-reboot -serial stdio

# Host-qemu twin of docker-run-blank (SCRUM-234): same zeroed disk, but in a
# real window on the host like `run`.
run-blank: docker-build
	rm -f build/exodoom_blank.img
	truncate -s "$$(stat -c %s build/exodoom_disk.img)" build/exodoom_blank.img
	qemu-system-x86_64 -m 256M -cdrom build/exodoom.iso -drive file=build/exodoom_blank.img,format=raw,if=ide,index=0 $(QEMU_DISK2_ARGS) -device intel-hda -device hda-output,audiodev=snd0 -audiodev none,id=snd0 $(QEMU_USB_ARGS) -no-reboot -serial stdio

clean:
	rm -rf build
