#include "stdlib.h"
#include "ctype.h"
#include "memory.h"

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
 * malloc/free/realloc are the kernel heap under its libc name.  kmalloc
 * (src/memory.c) dispatches to heap_alloc once the PMM is live and bump-
 * allocates before that, so a malloc() made before page_alloc_init() returns
 * permanent memory that free() will refuse with a serial warning rather than
 * corrupt.  Nothing on the boot path does that today -- Doom's allocations
 * all happen long after the PMM comes up -- but it is the one ordering rule
 * these wrappers inherit from kmalloc.
 */
void *malloc(size_t size) {
    return kmalloc(size);
}

void free(void *ptr) {
    kfree(ptr);
}

void *realloc(void *ptr, size_t size) {
    return krealloc(ptr, size);
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

static void swap_elems(char *a, char *b, size_t size) {
    while (size--) {
        char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

static void insertion_sort(char *base, size_t nmemb, size_t size, cmp_fn cmp) {
    for (size_t i = 1; i < nmemb; i++) {
        for (size_t j = i; j > 0 &&
                           cmp(base + (j - 1) * size, base + j * size) > 0; j--) {
            swap_elems(base + (j - 1) * size, base + j * size, size);
        }
    }
}

/*
 * Median-of-three quicksort with the pivot parked at lo.
 *
 * Two things matter more here than raw speed.  First, the pivot is compared
 * in place rather than copied, because element size is a runtime value and
 * there is no scratch buffer to copy it into -- hence parking it at lo and
 * partitioning only [lo+1, hi], so no swap can move it out from under the
 * comparisons.  Second, the recursion only ever descends into the *smaller*
 * partition and loops on the larger, which bounds stack depth at O(log n):
 * the kernel stack is 16 KiB (src/boot.s) and a naive quicksort recursing on
 * both sides would blow it on an adversarial input long before it finished.
 */
static void qsort_range(char *lo, size_t nmemb, size_t size, cmp_fn cmp) {
    while (nmemb > QSORT_INSERTION_THRESHOLD) {
        char *mid = lo + (nmemb / 2) * size;
        char *hi  = lo + (nmemb - 1) * size;

        /* Insertion-sort the three so that *lo <= *mid <= *hi, then park the
         * median at lo to be the pivot. */
        if (cmp(mid, lo) < 0) {
            swap_elems(mid, lo, size);
        }
        if (cmp(hi, mid) < 0) {
            swap_elems(hi, mid, size);
            if (cmp(mid, lo) < 0) {
                swap_elems(mid, lo, size);
            }
        }
        swap_elems(lo, mid, size);

        char *i = lo + size;
        char *j = hi;

        for (;;) {
            while (i <= j && cmp(i, lo) <= 0) {
                i += size;
            }
            while (i <= j && cmp(j, lo) >= 0) {
                j -= size;
            }
            if (i > j) {
                break;
            }
            swap_elems(i, j, size);
            i += size;
            j -= size;
        }

        /* j now points at the last element <= pivot; that is where the pivot
         * belongs.  j never walks below lo: the scan stops on i > j first. */
        swap_elems(lo, j, size);

        size_t left  = (size_t)(j - lo) / size;
        size_t right = nmemb - left - 1;

        if (left < right) {
            qsort_range(lo, left, size, cmp);
            lo = j + size;
            nmemb = right;
        } else {
            qsort_range(j + size, right, size, cmp);
            nmemb = left;
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
