.PHONY: docker-build docker-run docker-run-kernel docker-test docker-build-doom docker-link-doom switch-iwad clean

DEBUG ?= 0

# Which IWAD build.sh fetches/stages -- freedoom2 (auto-downloaded, default)
# or doom2 (the original commercial IWAD; must be staged by hand at
# build/doom2.wad first -- not auto-fetched, see docker/scripts/build.sh).
DOOM_IWAD ?= freedoom2

# Extra flags for the exodoom-build image's buildx invocation, e.g.
# "--cache-from type=gha --cache-to type=gha,mode=max" in CI. Empty locally.
BUILDX_CACHE_ARGS ?=

docker-build:
	docker buildx build $(BUILDX_CACHE_ARGS) --load -t exodoom-build -f docker/Dockerfile.build docker
	docker run --rm -e DEBUG=$(DEBUG) -e DOOM_IWAD=$(DOOM_IWAD) -v "$(PWD):/work" exodoom-build

docker-run: docker-build
	docker build -t exodoom-qemu -f docker/Dockerfile.qemu docker
	docker run --rm -it -v "$(PWD):/work" exodoom-qemu

# Optional: boot the kernel directly (no GRUB menu)
docker-run-kernel: docker-build
	docker build -t exodoom-qemu -f docker/Dockerfile.qemu docker
	docker run --rm -it -v "$(PWD):/work" exodoom-qemu \
	  'qemu-system-x86_64 -kernel build/exodoom -m 256M -no-reboot -display curses -serial mon:stdio'

docker-test:
	docker buildx build $(BUILDX_CACHE_ARGS) --load -t exodoom-build -f docker/Dockerfile.build docker
	docker run --rm -e DEBUG=$(DEBUG) -e TESTING=1 -e DOOM_IWAD=$(DOOM_IWAD) -v "$(PWD):/work" exodoom-build
	docker build -t exodoom-qemu -f docker/Dockerfile.qemu docker
	docker run --rm --entrypoint bash -v "$(PWD):/work" exodoom-qemu -lc '\
	  set -eu; \
	  rm -f /work/serial.log; \
	  timeout 120 qemu-system-x86_64 \
	  -cdrom build/exodoom.iso \
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
	docker run --rm -e DEBUG=$(DEBUG) -e TESTING=1 -e DOOM_IWAD=$(DOOM_IWAD) -v "$(PWD):/work" exodoom-build
	docker build -t exodoom-qemu -f docker/Dockerfile.qemu docker
	docker run --rm --entrypoint bash -v "$(PWD):/work" exodoom-qemu -lc '\
	  set -eu; \
	  rm -f /work/serial.log; \
	  timeout 120 qemu-system-x86_64 \
	  -cdrom build/exodoom.iso \
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

# Flip which IWAD is wired into src/grub.cfg + src/doomgeneric_exo.c
# (SCRUM-90) -- runs on the host, not in Docker, since it edits tracked
# source. Usage: make switch-iwad DOOM_IWAD=doom2
switch-iwad:
	docker/scripts/switch-iwad.sh $(DOOM_IWAD)

docker-run-debug: docker-build
	docker build -t exodoom-qemu -f docker/Dockerfile.qemu docker
	docker run --rm -it -p 1234:1234 -v "$(PWD):/work" exodoom-qemu \
	'qemu-system-x86_64 -cdrom build/exodoom.iso -m 256M -no-reboot -serial mon:stdio -s -S'

run: docker-build
	qemu-system-x86_64 -m 256M -cdrom build/exodoom.iso -no-reboot -serial stdio

clean:
	rm -rf build
