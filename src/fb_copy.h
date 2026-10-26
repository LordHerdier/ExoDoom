#pragma once
#include <stddef.h>
#include <stdint.h>

/*
 * fb_copy.h — bulk copy and clear for framebuffer-sized buffers (SCRUM-192).
 *
 * src/string.c's memcpy/memset move one byte per loop iteration, and the
 * kernel is built -mno-sse, so nothing vectorises them. A 1024x768x32
 * framebuffer is 3 MiB, so every compositor pass was ~3 million iterations.
 * Compositing runs at 60 Hz from syscall entry (src/fb_compositor.c), so on a
 * CPU slow enough that one pass takes longer than the 16.7 ms between passes
 * -- QEMU without KVM, under emulation -- *every* syscall composited, and a
 * LibOS making many syscalls in a row (the shell mapping its framebuffer one
 * page at a time) spent nearly all of its time copying.
 *
 * `rep movsq`/`rep stosq` do eight bytes per iteration with no SIMD state, so
 * the no-XMM-in-the-kernel invariant (docs/syscall_spec.md §3.4a) holds. They
 * rely on DF being clear, which the SysV ABI guarantees in C code and
 * IA32_FMASK guarantees on syscall entry (src/syscall.c).
 */

static inline void fb_copy_bytes(void *dst, const void *src, size_t n)
{
    size_t qwords = n / 8u;
    size_t tail   = n % 8u;

    __asm__ volatile("rep movsq"
                     : "+D"(dst), "+S"(src), "+c"(qwords) : : "memory");
    __asm__ volatile("rep movsb"
                     : "+D"(dst), "+S"(src), "+c"(tail) : : "memory");
}

static inline void fb_zero_bytes(void *dst, size_t n)
{
    size_t qwords = n / 8u;
    size_t tail   = n % 8u;

    __asm__ volatile("rep stosq"
                     : "+D"(dst), "+c"(qwords) : "a"((uint64_t)0) : "memory");
    __asm__ volatile("rep stosb"
                     : "+D"(dst), "+c"(tail) : "a"((uint64_t)0) : "memory");
}
