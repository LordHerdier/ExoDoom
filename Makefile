.PHONY: docker-build docker-run docker-run-kernel docker-test docker-build-doom docker-link-doom bench clean

DEBUG ?= 0

# 1 (default) boots `bench` with -enable-kvm; 0 boots it under plain TCG, for
# side-by-side comparison. See `bench`'s own comment for why this needs to be
# a host wall-clock measurement rather than trusting the serial log alone.
KVM ?= 1

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
docker-run-kernel: docker-build
	docker build -t exodoom-qemu -f docker/Dockerfile.qemu docker
	docker run --rm -it -v "$(PWD):/work" exodoom-qemu \
	  'qemu-system-x86_64 -kernel build/exodoom -m 256M -no-reboot -display curses -serial mon:stdio'

docker-test:
	docker buildx build $(BUILDX_CACHE_ARGS) --load -t exodoom-build -f docker/Dockerfile.build docker
	docker run --rm -e DEBUG=$(DEBUG) -e TESTING=1 -v "$(PWD):/work" exodoom-build
	docker build -t exodoom-qemu -f docker/Dockerfile.qemu docker
	docker run --rm --entrypoint bash -v "$(PWD):/work" exodoom-qemu -lc '\
	  set -eu; \
	  rm -f /work/serial.log; \
	  dd if=/dev/zero of=/work/build/ata_scratch.img bs=1M count=8 status=none; \
	  timeout 180 qemu-system-x86_64 \
	  -cdrom build/exodoom.iso \
	  -drive file=/work/build/ata_scratch.img,format=raw,if=ide \
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
	  dd if=/dev/zero of=/work/build/ata_scratch.img bs=1M count=8 status=none; \
	  timeout 180 qemu-system-x86_64 \
	  -cdrom build/exodoom.iso \
	  -drive file=/work/build/ata_scratch.img,format=raw,if=ide \
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
	'qemu-system-x86_64 -cdrom build/exodoom.iso -m 256M -no-reboot -serial mon:stdio -s -S'

run: docker-build
	qemu-system-x86_64 -m 256M -cdrom build/exodoom.iso -no-reboot -serial stdio

# Host-side benchmark run (SCRUM-87 follow-up) -- like `run` above, this is
# your own host's qemu, not the containerized one `docker-run*` use, because
# -enable-kvm needs /dev/kvm passed through and nothing here does that.
#
# The point of this target is the *host* wall-clock bracket around the whole
# session, written to <serial log>.timing. src/doom_profile.c's avg/max
# numbers are all measured with DG_GetTicksMs(), which is a clock the guest
# reads about *itself* (PIT ticks counted inside the VM whose speed is in
# question) -- so two serial logs that report the same "avg ms/frame" do not
# by themselves prove the two sessions took the same real time. Comparing
# each run's total guest ms covered (sum the "total" slot's frames * its
# window count, or just count doomgeneric_Tick() calls) against this
# target's host_elapsed_seconds is what actually answers that.
#
# KVM=0 for a TCG comparison run: `make bench KVM=0`.
bench: docker-build
	@log="serial_bench_$$(date +%Y%m%dT%H%M%S).log"; \
	timing="$$log.timing"; \
	kvmflag=""; \
	mode="TCG"; \
	if [ "$(KVM)" = "1" ]; then kvmflag="-enable-kvm"; mode="KVM"; fi; \
	echo "Booting build/exodoom.iso ($$mode) -- serial -> $$log"; \
	echo "Launch Doom from the shell and play as long as you want; close the"; \
	echo "QEMU window (or quit from the in-game menu) when you're done."; \
	start=$$(date +%s.%N); \
	qemu-system-x86_64 $$kvmflag -cdrom build/exodoom.iso -m 256M -no-reboot \
	  -serial file:$$log || true; \
	end=$$(date +%s.%N); \
	elapsed=$$(awk -v a="$$start" -v b="$$end" 'BEGIN { printf "%.3f", b - a }'); \
	{ \
	  echo "mode=$$mode"; \
	  echo "host_start_epoch=$$start"; \
	  echo "host_end_epoch=$$end"; \
	  echo "host_elapsed_seconds=$$elapsed"; \
	} | tee "$$timing"; \
	echo "serial log:  $$log"; \
	echo "timing file: $$timing"

clean:
	rm -rf build
