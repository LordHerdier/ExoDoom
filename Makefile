.PHONY: docker-build docker-run docker-run-kernel docker-test test clean

DEBUG ?= 0

docker-build:
	docker build -t exodoom-build -f docker/Dockerfile.build docker
	docker run --rm -e DEBUG=$(DEBUG) -v "$(PWD):/work" exodoom-build

docker-run: docker-build
	docker build -t exodoom-qemu -f docker/Dockerfile.qemu docker
	docker run --rm -it -v "$(PWD):/work" exodoom-qemu

# Optional: boot the kernel directly (no GRUB menu)
docker-run-kernel: docker-build
	docker build -t exodoom-qemu -f docker/Dockerfile.qemu docker
	docker run --rm -it -v "$(PWD):/work" exodoom-qemu \
	  'qemu-system-i386 -kernel build/exodoom -m 256M -no-reboot -display curses -serial mon:stdio'
docker-ci: docker-build
	docker build -t exodoom-qemu -f docker/Dockerfile.qemu docker
	docker run --rm --entrypoint bash -v "$(PWD):/work" exodoom-qemu -lc '\
	  set -eu; \
	  rm -f /work/serial.log; \
	  timeout 10 qemu-system-i386 \
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

docker-run-debug: docker-build
	docker build -t exodoom-qemu -f docker/Dockerfile.qemu docker
	docker run --rm -it -p 1234:1234 -v "$(PWD):/work" exodoom-qemu \
	'qemu-system-i386 -cdrom build/exodoom.iso -m 256M -no-reboot -serial mon:stdio -s -S'

run: docker-build
	qemu-system-i386 -m 256M -cdrom build/exodoom.iso -no-reboot -serial stdio

test:
	@mkdir -p build
	gcc -std=c99 -Wall -Wextra -fno-builtin -o build/test_string tests/test_string.c src/string.c
	./build/test_string

clean:
	rm -rf build
