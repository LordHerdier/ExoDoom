.PHONY: docker-build docker-run docker-run-kernel docker-test clean

docker-build:
	docker build -t exodoom-build -f docker/Dockerfile.build docker
	docker run --rm -v "$(PWD):/work" exodoom-build

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
	docker run --rm -v "$(PWD):/work" exodoom-qemu \
	  sh -c 'timeout 5 qemu-system-i386 \
	    -cdrom build/exodoom.iso \
	    -m 256M \
			-nographic \
	    -vga std \
	    -serial stdio \
	    -device isa-debug-exit,iobase=0xf4,iosize=0x04'

clean:
	rm -rf build
