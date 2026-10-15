.PHONY: docker-build docker-run docker-run-kernel docker-test docker-build-doom docker-link-doom clean

DEBUG ?= 0

# Extra flags for the exodoom-build image's buildx invocation, e.g.
# "--cache-from type=gha --cache-to type=gha,mode=max" in CI. Empty locally.
BUILDX_CACHE_ARGS ?=

docker-build:
	docker buildx build $(BUILDX_CACHE_ARGS) --load -t exodoom-build -f docker/Dockerfile.build docker
	docker run --rm -e DEBUG=$(DEBUG) -v "$(PWD):/work" exodoom-build

docker-run: docker-build
	docker build -t exodoom-qemu -f docker/Dockerfile.qemu docker
	docker run --rm -it -v "$(PWD):/work" exodoom-qemu

# Optional: boot the kernel directly (no GRUB menu)
#
# -drive ...build/exodoom_disk.img: the ExoFS volume holding freedoom2.wad
# (SCRUM-190, docker/scripts/build.sh step [1b/7]), replacing the old GRUB
# multiboot2 module -- see src/syscall_launch.c. `-kernel` bypasses GRUB
# entirely, but the drive is attached the same way as every other
# normal-boot target below.
docker-run-kernel: docker-build
	docker build -t exodoom-qemu -f docker/Dockerfile.qemu docker
	docker run --rm -it -v "$(PWD):/work" exodoom-qemu \
	  'qemu-system-x86_64 -kernel build/exodoom -drive file=build/exodoom_disk.img,format=raw,if=ide -m 256M -device intel-hda -device hda-output,audiodev=snd0 -audiodev none,id=snd0 -no-reboot -display curses -serial mon:stdio'

# build/exodoom_disk.img (built by docker/scripts/build.sh step [1b/7],
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
	docker run --rm -e DEBUG=$(DEBUG) -e TESTING=1 -v "$(PWD):/work" exodoom-build
	docker build -t exodoom-qemu -f docker/Dockerfile.qemu docker
	docker run --rm --entrypoint bash -v "$(PWD):/work" exodoom-qemu -lc '\
	  set -eu; \
	  rm -f /work/serial.log; \
	  timeout 180 qemu-system-x86_64 \
	  -cdrom build/exodoom.iso \
	  -drive file=/work/build/exodoom_disk.img,format=raw,if=ide \
	  -device intel-hda \
	  -device hda-output,audiodev=snd0 \
	  -audiodev none,id=snd0 \
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
	docker run --rm -e DEBUG=$(DEBUG) -e TESTING=1 -v "$(PWD):/work" exodoom-build
	docker build -t exodoom-qemu -f docker/Dockerfile.qemu docker
	docker run --rm --entrypoint bash -v "$(PWD):/work" exodoom-qemu -lc '\
	  set -eu; \
	  rm -f /work/serial.log; \
	  timeout 180 qemu-system-x86_64 \
	  -cdrom build/exodoom.iso \
	  -drive file=/work/build/exodoom_disk.img,format=raw,if=ide \
	  -device intel-hda \
	  -device hda-output,audiodev=snd0 \
	  -audiodev none,id=snd0 \
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
	'qemu-system-x86_64 -cdrom build/exodoom.iso -drive file=build/exodoom_disk.img,format=raw,if=ide -m 256M -device intel-hda -device hda-output,audiodev=snd0 -audiodev none,id=snd0 -no-reboot -serial mon:stdio -s -S'

# Boots on the HOST's qemu rather than in the container, which is the only
# place a speaker exists -- swap `-audiodev none,id=snd0` below for
# `-audiodev pa,id=snd0` (or alsa/sdl/coreaudio) to actually hear SCRUM-210's
# test tone. Everything under docker-* keeps the null backend: it still runs
# the full BDL DMA and completion-interrupt path, which is what the KUnit
# suite asserts on, and headless Docker has no audio backend to hand.
run: docker-build
	qemu-system-x86_64 -m 256M -cdrom build/exodoom.iso -drive file=build/exodoom_disk.img,format=raw,if=ide -device intel-hda -device hda-output,audiodev=snd0 -audiodev none,id=snd0 -no-reboot -serial stdio

clean:
	rm -rf build
