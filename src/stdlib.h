#pragma once
#include <stddef.h>

/*
 * Freestanding stdlib.h (SCRUM-30).
 *
 * The subset of <stdlib.h> doomgeneric needs from the allocation, numeric
 * and sorting families.  See docs/syscall_spec.md sec2.2 for the call-count
 * audit that picked these; `calloc`, `exit`, `abort`, `atexit`, `getenv`,
 * `atof` and `system` are still Todo there.
 *
 * malloc/free/realloc are thin wrappers over the kernel heap by way of
 * kmalloc/kfree/krealloc (src/memory.c -> src/heap.c, SCRUM-25).  There is
 * no separate user-space heap yet: the LibOS runs in the kernel's own
 * address space until SCRUM-47, so both share one allocator.
 */

/* rand() returns a value in [0, RAND_MAX]. 32767 is the C-standard minimum
 * and what the reference LCG below produces. */
#define RAND_MAX 32767

void *malloc(size_t size);
void  free(void *ptr);
void *realloc(void *ptr, size_t size);

int atoi(const char *nptr);
int abs(int j);

/*
 * ---------------------------------------------------------------------------
 * SCRUM-65.
 * ---------------------------------------------------------------------------
 */

/* Zero-initialised allocation. One call site under src/doom/, and it was
 * one of the eight functions docs/libc_audit.md sec3.2 found being called
 * with NO declaration in scope -- so its pointer return was truncated to 32
 * bits at the call site, which would have presented as heap corruption. */
void *calloc(size_t nmemb, size_t size);

/*
 * exit / abort.
 *
 * Neither returns. On a ring-3 LibOS both go through exo_exit (#20), which
 * releases the context's pages and framebuffer binding via revoke_all();
 * in a kernel build there is no context to exit, so both halt with
 * interrupts masked after flushing COM1. `status` is reported on serial
 * either way, since it is frequently the only thing distinguishing a clean
 * quit from a failure.
 */
void exit(int status);
void abort(void);

/*
 * system(): always fails, and always will.
 *
 * There is no process model to run a command under. Returning -1 is what
 * the standard says to do when a child cannot be created, and it is what
 * i_system.c's ZenityAvailable() already treats as "not available".
 * Returning 0 would claim the command had run successfully.
 */
int system(const char *command);

/*
 * atof: decimal text to double.
 *
 * NOT available in a kernel build -- it cannot be, since the x86_64 SysV ABI
 * returns double in xmm0 and the kernel compiles -mno-sse. The engine behind
 * it, exo_parse_f64() in src/fpconv.h, is integer-only and IS available (and
 * unit-tested) everywhere; this declaration is the thin ring-3 adapter over
 * it. Its one call site is m_config.c:1766.
 */
#ifndef EXO_KERNEL
double atof(const char *nptr);
#endif

int  rand(void);
void srand(unsigned int seed);

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *));
