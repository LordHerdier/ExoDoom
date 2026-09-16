#include "stdlib.h"
#include "ctype.h"

#ifdef EXO_KERNEL
#include "memory.h"
#else
#include "libos_heap.h"
#endif

#include "fpconv.h"  /* exo_parse_f64 -- atof's integer-only engine (SCRUM-65) */
#include "stdio.h"   /* printf: exit() reports its status on serial */
#include "string.h"  /* memset: calloc */

#ifdef EXO_KERNEL
#include "serial.h"  /* serial_flush before exit()'s halt */
#else
#include "exo_syscall.h"  /* exo_exit (#20) */
#endif

#include <limits.h>
#include <stdint.h>

/*
 * Freestanding stdlib.h implementation (SCRUM-30).
 *
 * Three unrelated families live here because <stdlib.h> puts them together:
 * the allocation wrappers, the small numeric conversions, and rand/qsort.
 * See docs/syscall_spec.md sec2.2 for which doomgeneric call sites need what.
 */

/* ---------------------------------------------------------------- memory */

/*
 * malloc/free/realloc back onto two different allocators depending on which
 * side of the kernel/LibOS boundary this translation unit is built for
 * (SCRUM-51) -- the same EXO_KERNEL switch src/exo_syscall.h already uses to
 * pick between the kernel and LibOS views of the syscall ABI:
 *
 *   - EXO_KERNEL (kernel builds, and every existing kernel test suite
 *     compiled through the shared loop): kmalloc/kfree/krealloc, the kernel
 *     heap under its libc name.  kmalloc (src/memory.c) dispatches to
 *     heap_alloc once the PMM is live and bump-allocates before that, so a
 *     malloc() made before page_alloc_init() returns permanent memory that
 *     free() will refuse with a serial warning rather than corrupt.
 *   - otherwise (the ring-3 LibOS link target introduced by SCRUM-173):
 *     libos_heap_alloc/_free/_realloc (src/libos_heap.c, SCRUM-38), which
 *     gets its pages from libos_page_alloc() -- itself re-pointed at the
 *     real `syscall` instruction stubs for this same build (see
 *     src/libos_page_alloc.c) rather than the in-kernel dispatch call it
 *     uses under EXO_KERNEL.  This is the wiring SCRUM-51's acceptance
 *     criterion ("malloc ... works from ring 3 through the syscall
 *     instruction") asks for -- libos_heap.c and libos_page_alloc.c already
 *     existed (SCRUM-37/-38) but nothing called them from compiled code
 *     running at LIBOS_LAUNCH_CODE_VADDR until this ticket.
 */
void *malloc(size_t size) {
#ifdef EXO_KERNEL
    return kmalloc(size);
#else
    return libos_heap_alloc(size);
#endif
}

void free(void *ptr) {
#ifdef EXO_KERNEL
    kfree(ptr);
#else
    libos_heap_free(ptr);
#endif
}

void *realloc(void *ptr, size_t size) {
#ifdef EXO_KERNEL
    return krealloc(ptr, size);
#else
    return libos_heap_realloc(ptr, size);
#endif
}

/* --------------------------------------------------------------- numeric */

/*
 * atoi is strtol(nptr, NULL, 10) truncated to int, minus the error
 * reporting.  C leaves overflow undefined; this saturates at INT_MAX /
 * INT_MIN instead, which is deterministic and a better answer than a wrap
 * for the config and command-line parsing Doom uses it for (14 call sites).
 */
int atoi(const char *nptr) {
    const char *p = nptr;
    int neg = 0;

    if (p == NULL) {
        return 0;
    }

    while (isspace((unsigned char)*p)) {
        p++;
    }

    if (*p == '-' || *p == '+') {
        neg = (*p == '-');
        p++;
    }

    /* Accumulating in long (64-bit here) keeps the cap comparison exact:
     * -(long)INT_MIN is 2147483648, which does not fit in an int. */
    long cap = neg ? -(long)INT_MIN : (long)INT_MAX;
    long acc = 0;

    while (isdigit((unsigned char)*p)) {
        /* Stop accumulating once past the cap so a long digit run cannot
         * overflow the accumulator itself; the clamp below finishes the job. */
        if (acc <= cap) {
            acc = acc * 10 + (*p - '0');
        }
        p++;
    }

    if (acc > cap) {
        acc = cap;
    }

    return neg ? (int)-acc : (int)acc;
}

/*
 * abs(INT_MIN) is undefined in C -- negating it overflows.  Doing the negate
 * in unsigned arithmetic makes the result well defined (INT_MIN comes back
 * out) instead of leaving -O2 free to assume it never happens.
 */
int abs(int j) {
    return j < 0 ? (int)(0u - (unsigned int)j) : j;
}

/* ------------------------------------------------------------------ rand */

/*
 * The C standard's own reference LCG, pinned to uint32_t so the sequence is
 * the well-known one (16838, 5758, 10113, ... for seed 1) rather than
 * whatever a 64-bit accumulator would produce.  Seeding is the whole point
 * of the acceptance criterion: srand(s) followed by rand() must replay the
 * same sequence every time, which tests/kernel/test_stdlib_k.c asserts
 * against that fixed vector.
 *
 * The initial state is 1 because C specifies rand() to behave as if srand(1)
 * had been called.  Doom does not use this for gameplay -- P_Random has its
 * own table -- so LCG quality is not a concern here.
 */
static uint32_t rand_state = 1;

int rand(void) {
    rand_state = rand_state * 1103515245u + 12345u;
    /* Bits 30..16: the low bits of an LCG have short periods, the top bit is
     * dropped to keep the result in [0, RAND_MAX]. */
    return (int)((rand_state >> 16) & 0x7FFFu);
}

void srand(unsigned int seed) {
    rand_state = (uint32_t)seed;
}

/* ----------------------------------------------------------------- qsort */

typedef int (*cmp_fn)(const void *, const void *);

/* Partitions at or below this many elements go to insertion sort, which wins
 * on small runs and removes the recursion's base-case entirely. */
#define QSORT_INSERTION_THRESHOLD 8

/*
 * Byte-wise exchange of two non-overlapping runs.  One element is just `size`
 * bytes, so this doubles as the block swap the three-way partition uses to
 * rotate its equal-key runs into the middle.  n == 0 must be a no-op: that
 * happens whenever one of the two runs a block swap names is empty.
 */
static void swap_bytes(char *a, char *b, size_t n) {
    for (size_t k = 0; k < n; k++) {
        char t = a[k];
        a[k] = b[k];
        b[k] = t;
    }
}

static void insertion_sort(char *base, size_t nmemb, size_t size, cmp_fn cmp) {
    for (size_t i = 1; i < nmemb; i++) {
        for (size_t j = i; j > 0 &&
                           cmp(base + (j - 1) * size, base + j * size) > 0; j--) {
            swap_bytes(base + (j - 1) * size, base + j * size, size);
        }
    }
}

/*
 * Median-of-three quicksort, three-way (Bentley-McIlroy) partition, pivot
 * parked at lo.
 *
 * Three properties matter more here than raw speed.
 *
 * First, the pivot is compared in place rather than copied, because element
 * size is a runtime value and there is no scratch buffer to copy it into --
 * hence parking it at lo and partitioning only [lo+1, hi], so no swap can
 * move it out from under the comparisons.  Nothing below ever writes to lo
 * while the scans are running.
 *
 * Second, the recursion only ever descends into the *smaller* partition and
 * loops on the larger, which bounds stack depth at O(log n): the kernel stack
 * is 16 KiB (src/boot.s) and a naive quicksort recursing on both sides would
 * blow it on an adversarial input long before it finished.  Measured depth at
 * n = 1,000,000 is 18 for sorted/reverse input against a log2 n of 20.
 *
 * Third -- SCRUM-171 -- keys equal to the pivot are collected rather than
 * swept into one side.  The two-way Hoare partition this replaced scanned
 * with `cmp(i, lo) <= 0` / `cmp(j, lo) >= 0`, so both pointers ran straight
 * over equal keys and split n-1/0 on any input with few distinct values, i.e.
 * O(n^2): all-equal input at n = 200,000 cost 2.0e10 comparisons (5882x
 * n log n, 29 s) before this.  Here each scan stops on an equal key and parks
 * it at the end it came from, so the scans meet in the middle and the loop
 * ends with the array in four runs:
 *
 *     lo        pa        pb pc        pd        pn
 *     |  == P   |   < P   | ?  |  > P  |  == P   |
 *
 * with the unscanned ? region empty (pb == pc + size).  The two block swaps
 * below rotate the equal runs inward, into the position they already belong
 * in, so both recursions exclude them: all-equal input becomes a single O(n)
 * pass (200,001 comparisons at n = 200,000) and k distinct values cost
 * O(n log k).  Sorted and reverse input are unaffected; random input is ~20%
 * cheaper because duplicate keys stop being re-partitioned.
 */
static void qsort_range(char *lo, size_t nmemb, size_t size, cmp_fn cmp) {
    while (nmemb > QSORT_INSERTION_THRESHOLD) {
        char *pn  = lo + nmemb * size;         /* one past the last element */
        char *mid = lo + (nmemb / 2) * size;
        char *hi  = pn - size;

        /* Insertion-sort the three so that *lo <= *mid <= *hi, then park the
         * median at lo to be the pivot. */
        if (cmp(mid, lo) < 0) {
            swap_bytes(mid, lo, size);
        }
        if (cmp(hi, mid) < 0) {
            swap_bytes(hi, mid, size);
            if (cmp(mid, lo) < 0) {
                swap_bytes(mid, lo, size);
            }
        }
        swap_bytes(lo, mid, size);

        char *pa = lo + size;      /* one past the leading  == P run       */
        char *pb = lo + size;      /* first unscanned element from the left */
        char *pc = hi;             /* last unscanned element from the right */
        char *pd = hi;             /* one before the trailing == P run      */

        for (;;) {
            /* r is only read on the iteration that assigned it: && stops the
             * comparison from running once the scan pointers have crossed. */
            int r = 0;

            while (pb <= pc && (r = cmp(pb, lo)) <= 0) {
                if (r == 0) {
                    swap_bytes(pa, pb, size);
                    pa += size;
                }
                pb += size;
            }
            while (pb <= pc && (r = cmp(pc, lo)) >= 0) {
                if (r == 0) {
                    swap_bytes(pc, pd, size);
                    pd -= size;
                }
                pc -= size;
            }
            if (pb > pc) {
                break;
            }
            swap_bytes(pb, pc, size);
            pb += size;
            pc -= size;
        }

        /* Byte lengths of the four runs; they tile [lo, pn) exactly. */
        size_t lead_eq  = (size_t)(pa - lo);           /* holds the pivot */
        size_t less     = (size_t)(pb - pa);
        size_t greater  = (size_t)(pd - pc);
        size_t trail_eq = (size_t)(pn - pd) - size;
        size_t s;

        /* Rotate each == P run inward past the neighbouring < P / > P run.
         * Moving only min(the two lengths) bytes is what keeps the source and
         * destination of each swap from overlapping. */
        s = lead_eq < less ? lead_eq : less;
        swap_bytes(lo, pb - s, s);

        s = greater < trail_eq ? greater : trail_eq;
        swap_bytes(pb, pn - s, s);

        /* The == P run in the middle is in final position and belongs to
         * neither side.  It always holds at least the pivot, so both sides
         * are strictly smaller than nmemb and the loop always makes progress. */
        char  *right   = pn - greater;
        size_t left_n  = less / size;
        size_t right_n = greater / size;

        if (left_n < right_n) {
            qsort_range(lo, left_n, size, cmp);
            lo = right;
            nmemb = right_n;
        } else {
            qsort_range(right, right_n, size, cmp);
            nmemb = left_n;
        }
    }

    insertion_sort(lo, nmemb, size, cmp);
}

void qsort(void *base, size_t nmemb, size_t size, cmp_fn compar) {
    /* A zero element size would make every pointer step a no-op and never
     * terminate; a null comparator has nothing to sort by. */
    if (base == NULL || compar == NULL || size == 0 || nmemb < 2) {
        return;
    }
    qsort_range((char *)base, nmemb, size, compar);
}

/*
 * ===========================================================================
 * SCRUM-65. See src/stdlib.h for what each of these promises.
 * ===========================================================================
 */

void *calloc(size_t nmemb, size_t size)
{
    size_t total;
    void  *p;

    if (nmemb == 0 || size == 0) {
        /* Either a NULL or a unique freeable pointer is conforming. malloc(0)
         * here already picks one; deferring to it keeps the two consistent
         * rather than inventing a second convention. */
        return malloc(0);
    }

    /* The overflow check is the whole reason calloc exists as something other
     * than malloc+memset: nmemb * size wrapping produces a small allocation
     * that the caller then writes nmemb*size bytes into. Checking the
     * division form cannot itself overflow. */
    if (nmemb > (size_t)-1 / size) {
        return NULL;
    }

    total = nmemb * size;
    p     = malloc(total);

    if (p != NULL) {
        memset(p, 0, total);
    }

    return p;
}

int system(const char *command)
{
    (void)command;

    /* A NULL command asks "is a shell available?" -- and the answer is no,
     * which is the same -1... except the standard says return 0 for that
     * probe when no interpreter exists. Both branches collapse to "no shell",
     * so reporting it the way each caller expects costs one comparison. */
    return (command == NULL) ? 0 : -1;
}

void exit(int status)
{
    printf("exit(%d)\n", status);

#ifdef EXO_KERNEL
    serial_flush();
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
#else
    exo_exit(status);
    /* exo_exit spins internally, but only if the syscall is bound; a LibOS
     * that kept running after announcing its own exit would be worse than
     * one that stops. */
    for (;;) {
        __asm__ volatile("pause" ::: "memory");
    }
#endif
}

void abort(void)
{
    /* SIGABRT's conventional exit status, and the one a shell would report
     * -- kept even though nothing here has a shell, because it is the value
     * anyone reading the serial line will recognise. */
    exit(134);
}

#ifndef EXO_KERNEL
double atof(const char *nptr)
{
    /*
     * The thin adapter src/fpconv.h describes: every part of this that can
     * be wrong -- the exponent, the rounding, the overflow and underflow
     * edges -- is in exo_parse_f64(), which is integer-only and is driven
     * directly by tests/kernel/test_fpconv_k.c from ring 0. All that happens
     * here is moving 8 bytes.
     */
    uint64_t bits = exo_parse_f64(nptr, NULL);
    double   d;

    __builtin_memcpy(&d, &bits, sizeof d);

    return d;
}
#endif
