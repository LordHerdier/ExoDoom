.PHONY: docker-build docker-run docker-run-kernel clean

docker-build:
	docker build -t exodoom-build -f Dockerfile.build .
	docker run --rm -v "$(PWD):/work" exodoom-build

docker-run: docker-build
	docker build -t exodoom-qemu -f Dockerfile.qemu .
	docker run --rm -it -v "$(PWD):/work" exodoom-qemu

# Optional: boot the kernel directly (no GRUB menu)
docker-run-kernel: docker-build
	docker build -t exodoom-qemu -f Dockerfile.qemu .
	docker run --rm -it -v "$(PWD):/work" exodoom-qemu \
	  'qemu-system-i386 -kernel build/exodoom -m 256M -no-reboot -display curses -serial mon:stdio'

clean:
	rm -rf build
