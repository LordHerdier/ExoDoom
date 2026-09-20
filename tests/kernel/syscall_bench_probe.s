/*
 * syscall_bench_probe.s — round-trip cycle counters for SCRUM-60.
 *
 * Three ring-3 entry points, launched the modern way (SCRUM-49/-50):
 * libos_build_image() copies one of these into a fresh LibOS window and
 * libos_enter() iretqs to it, exactly like tests/kernel/libos_main_probe.s.
 * tests/kernel/ring3_probe.s's older ring3_run()/ring3_escape() pair is
 * NOT used here -- src/vmm.c (SCRUM-55) re-exposes only ring3_probe's and
 * tss_fault_probe's own byte ranges as user-executable, "the sole,
 * explicitly-scoped legacy exception"; a probe linked anywhere else would
 * fault the instant CPL 3 tried to fetch from it.
 *
 * Each probe reads its parameters from a small struct at the fixed
 * immediate LIBOS_LAUNCH_DATA_VADDR (never by address of its own compiled
 * location, per the PIC rule libos_build_image()'s header comment states),
 * times `iterations` back-to-back syscalls with `rdtsc`, and hands the
 * cycle delta back through LIBOS_RETURN_SYSCALL_NUM the same way every
 * other launch probe in this tree escapes. CR4.TSD is never set (see
 * src/boot.s), so `rdtsc` already executes at CPL 3 with no kernel change.
 *
 * The exact byte layout of the params struct at DATA_VADDR is each probe's
 * own contract with its C driver (tests/kernel/test_syscall_bench_k.c) --
 * documented above each entry point rather than shared via a struct type,
 * since assembly only needs the offsets, not the type.
 */

#include "exo_syscall.h"
#include "libos_launch.h"

.code64

.set DATA_VADDR,      LIBOS_LAUNCH_DATA_VADDR
.set SYS_LIBOS_RETURN, LIBOS_RETURN_SYSCALL_NUM
.set SYS_PAGE_ALLOC,   EXO_SYS_PAGE_ALLOC
.set SYS_PAGE_FREE,    EXO_SYS_PAGE_FREE
.set SYS_PAGE_MAP,     EXO_SYS_PAGE_MAP
.set SYS_PAGE_UNMAP,   EXO_SYS_PAGE_UNMAP

.section .text

/*
 * bench_probe_simple — loop a single fixed syscall N times.
 *
 * Params at DATA_VADDR (struct bench_simple_params_t, test_syscall_bench_k.c):
 *   0  sysnum
 *   8  a1
 *   16 a2
 *   24 a3
 *   32 a4
 *   40 iterations
 *
 * Covers every case that takes at most 4 args and needs no per-iteration
 * bookkeeping: exo_get_ticks, exo_kbd_poll (empty ring), exo_serial_write
 * (len=0), exo_fb_acquire (re-acquire), and the exo_mouse_poll -ENOSYS
 * floor -- see test_syscall_bench_k.c for why each of those is safe to
 * call blind, back to back, with no per-call result handling.
 */
.global bench_probe_simple
.global bench_probe_simple_end
bench_probe_simple:
    movabsq $DATA_VADDR, %rbx

    rdtsc
    shlq $32, %rdx
    orq  %rdx, %rax
    movq %rax, %rbp                 /* start cycle count */

    movq 40(%rbx), %r12             /* iterations */
    testq %r12, %r12
    jz 2f
1:
    movq 0(%rbx), %rax              /* sysnum */
    movq 8(%rbx), %rdi              /* a1 */
    movq 16(%rbx), %rsi             /* a2 */
    movq 24(%rbx), %rdx             /* a3 */
    movq 32(%rbx), %r10             /* a4 */
    xorq %r8, %r8
    xorq %r9, %r9
    syscall
    decq %r12
    jnz 1b
2:
    rdtsc
    shlq $32, %rdx
    orq  %rdx, %rax
    subq %rbp, %rax                 /* elapsed cycles for N syscalls */

    movq %rax, %rdi
    movq $SYS_LIBOS_RETURN, %rax
    syscall
    ud2                              /* unreachable */
bench_probe_simple_end:

/*
 * bench_probe_page_alloc_free — loop exo_page_alloc()/exo_page_free() N
 * times, freeing each page immediately after allocating it so the PMM ends
 * exactly where it started.
 *
 * Params at DATA_VADDR (struct bench_pair_params_t):
 *   0  iterations
 */
.global bench_probe_page_alloc_free
.global bench_probe_page_alloc_free_end
bench_probe_page_alloc_free:
    movabsq $DATA_VADDR, %rbx

    rdtsc
    shlq $32, %rdx
    orq  %rdx, %rax
    movq %rax, %rbp

    movq 0(%rbx), %r12               /* iterations */
    testq %r12, %r12
    jz 2f
1:
    movq $SYS_PAGE_ALLOC, %rax
    syscall                          /* rax = paddr, or a negative error */
    movq %rax, %rdi                  /* pass straight to page_free */
    movq $SYS_PAGE_FREE, %rax
    syscall
    decq %r12
    jnz 1b
2:
    rdtsc
    shlq $32, %rdx
    orq  %rdx, %rax
    subq %rbp, %rax                  /* elapsed cycles for 2N syscalls */

    movq %rax, %rdi
    movq $SYS_LIBOS_RETURN, %rax
    syscall
    ud2
bench_probe_page_alloc_free_end:

/*
 * bench_probe_page_map_unmap — loop exo_page_map()/exo_page_unmap() N times
 * against one page the C driver has already allocated, mapping and
 * unmapping the same vaddr/paddr pair every iteration.
 *
 * Params at DATA_VADDR (struct bench_map_params_t):
 *   0  vaddr
 *   8  paddr
 *   16 flags
 *   24 iterations
 */
.global bench_probe_page_map_unmap
.global bench_probe_page_map_unmap_end
bench_probe_page_map_unmap:
    movabsq $DATA_VADDR, %rbx

    rdtsc
    shlq $32, %rdx
    orq  %rdx, %rax
    movq %rax, %rbp

    movq 0(%rbx), %r13                /* vaddr */
    movq 8(%rbx), %r14                /* paddr */
    movq 16(%rbx), %r15               /* flags */
    movq 24(%rbx), %r12               /* iterations */
    testq %r12, %r12
    jz 2f
1:
    movq $SYS_PAGE_MAP, %rax
    movq %r13, %rdi
    movq %r14, %rsi
    movq %r15, %rdx
    syscall
    movq $SYS_PAGE_UNMAP, %rax
    movq %r13, %rdi
    syscall
    decq %r12
    jnz 1b
2:
    rdtsc
    shlq $32, %rdx
    orq  %rdx, %rax
    subq %rbp, %rax                   /* elapsed cycles for 2N syscalls */

    movq %rax, %rdi
    movq $SYS_LIBOS_RETURN, %rax
    syscall
    ud2
bench_probe_page_map_unmap_end:
