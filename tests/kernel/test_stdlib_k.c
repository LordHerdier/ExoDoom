/*
 * test_stdlib_k.c -- freestanding <stdlib.h> shim (SCRUM-30).
 *
 * Covers the two acceptance criteria explicitly:
 *   - malloc routes to the kernel heap  (test_malloc_routes_to_kernel_heap)
 *   - rand is deterministic given a seed (test_rand_seed_1_reference_vector,
 *     test_rand_is_deterministic_per_seed)
 * plus free/realloc, atoi, abs and qsort.
 */

#include "kunit.h"
#include "stdlib.h"
#include "heap.h"
#include "string.h"

#include <limits.h>
#include <stdint.h>

/* ---------------------------------------------------------------- memory */

static void test_malloc_nonnull_and_aligned(void) {
    void *p = malloc(37);
    CU_ASSERT_PTR_NOT_NULL(p);
    /* The kernel heap aligns every payload to 16 bytes (HEAP_ALIGN). */
    CU_ASSERT_EQUAL((uintptr_t)p % 16, 0);
    free(p);
}

/*
 * The acceptance criterion: malloc/free must be the kernel heap, not a
 * separate pool.  Proven by having heap_alloc() land on a block that malloc
 * allocated and free() released.
 *
 * That identity is exact, not a coincidence of timing.  heap.c coalesces on
 * free, so no free block ever has a free physical predecessor -- mid's block
 * therefore cannot merge backwards and keeps its start address.  And when
 * malloc(mid) picked that block, first-fit had already established that no
 * earlier free block of this size existed; allocating hi afterwards only
 * consumed space at or above it.  So the first fit after free(mid) is mid.
 */
static void test_malloc_routes_to_kernel_heap(void) {
    void *lo  = malloc(96);
    void *mid = malloc(96);
    void *hi  = malloc(96);      /* lo and hi stay held as in-use guards */
    CU_ASSERT_PTR_NOT_NULL(lo);
    CU_ASSERT_PTR_NOT_NULL(mid);
    CU_ASSERT_PTR_NOT_NULL(hi);

    free(mid);

    void *again = heap_alloc(96);
    CU_ASSERT_EQUAL(mid, again);

    /* The reverse direction too: free() accepts a heap_alloc() block. */
    free(again);
    free(lo);
    free(hi);
}

static void test_malloc_blocks_dont_overlap(void) {
    unsigned char *a = (unsigned char *)malloc(64);
    unsigned char *b = (unsigned char *)malloc(128);
    CU_ASSERT_PTR_NOT_NULL(a);
    CU_ASSERT_PTR_NOT_NULL(b);

    memset(a, 0xAA, 64);
    memset(b, 0xBB, 128);

    int a_intact = 1;
    for (int i = 0; i < 64; i++) {
        if (a[i] != 0xAA) a_intact = 0;
    }
    CU_ASSERT_TRUE(a_intact);

    free(a);
    free(b);
}

static void test_free_null_is_noop(void) {
    free(NULL);          /* must not fault or complain */
    CU_ASSERT_TRUE(1);
}

static void test_realloc_preserves_data(void) {
    unsigned char *p = (unsigned char *)malloc(32);
    CU_ASSERT_PTR_NOT_NULL(p);
    for (int i = 0; i < 32; i++) p[i] = (unsigned char)(i + 1);

    unsigned char *q = (unsigned char *)realloc(p, 512);
    CU_ASSERT_PTR_NOT_NULL(q);

    int ok = 1;
    for (int i = 0; i < 32; i++) {
        if (q[i] != (unsigned char)(i + 1)) ok = 0;
    }
    CU_ASSERT_TRUE(ok);

    free(q);
}

static void test_realloc_null_and_zero(void) {
    void *p = realloc(NULL, 64);      /* realloc(NULL, n) == malloc(n) */
    CU_ASSERT_PTR_NOT_NULL(p);

    void *q = realloc(p, 0);          /* realloc(p, 0) frees and returns NULL */
    CU_ASSERT_PTR_NULL(q);
}

/* ------------------------------------------------------------------ atoi */

static void test_atoi_basic(void) {
    CU_ASSERT_EQUAL(atoi("0"), 0);
    CU_ASSERT_EQUAL(atoi("7"), 7);
    CU_ASSERT_EQUAL(atoi("42"), 42);
    CU_ASSERT_EQUAL(atoi("1234567"), 1234567);
}

static void test_atoi_sign_and_whitespace(void) {
    CU_ASSERT_EQUAL(atoi("-42"), -42);
    CU_ASSERT_EQUAL(atoi("+42"), 42);
    CU_ASSERT_EQUAL(atoi("   13"), 13);
    CU_ASSERT_EQUAL(atoi("\t\n -5"), -5);
    CU_ASSERT_EQUAL(atoi("-0"), 0);
}

static void test_atoi_stops_at_garbage(void) {
    CU_ASSERT_EQUAL(atoi("12abc"), 12);
    CU_ASSERT_EQUAL(atoi("3.9"), 3);        /* stops at '.', as atoi must */
    CU_ASSERT_EQUAL(atoi("abc"), 0);
    CU_ASSERT_EQUAL(atoi(""), 0);
    CU_ASSERT_EQUAL(atoi("- 5"), 0);        /* space after sign: no digits */
}

static void test_atoi_saturates_on_overflow(void) {
    CU_ASSERT_EQUAL(atoi("2147483647"), INT_MAX);
    CU_ASSERT_EQUAL(atoi("-2147483648"), INT_MIN);
    /* C leaves overflow undefined; this shim clamps rather than wrapping. */
    CU_ASSERT_EQUAL(atoi("2147483648"), INT_MAX);
    CU_ASSERT_EQUAL(atoi("99999999999999999999"), INT_MAX);
    CU_ASSERT_EQUAL(atoi("-99999999999999999999"), INT_MIN);
}

/* ------------------------------------------------------------------- abs */

static void test_abs(void) {
    CU_ASSERT_EQUAL(abs(0), 0);
    CU_ASSERT_EQUAL(abs(5), 5);
    CU_ASSERT_EQUAL(abs(-5), 5);
    CU_ASSERT_EQUAL(abs(INT_MAX), INT_MAX);
    /* abs(INT_MIN) has no representable answer; the shim wraps in unsigned
     * arithmetic rather than invoking signed-overflow UB. */
    CU_ASSERT_EQUAL(abs(INT_MIN), INT_MIN);
}

/* ------------------------------------------------------------------ rand */

static void test_rand_seed_1_reference_vector(void) {
    /* The C standard's reference LCG, seed 1.  Pinning the vector catches a
     * change to the generator, not just to its determinism. */
    static const int expected[6] = { 16838, 5758, 10113, 17515, 31051, 5627 };

    srand(1);
    for (int i = 0; i < 6; i++) {
        CU_ASSERT_EQUAL(rand(), expected[i]);
    }
}

static void test_rand_is_deterministic_per_seed(void) {
    int first[32], second[32];

    srand(12345);
    for (int i = 0; i < 32; i++) first[i] = rand();

    srand(12345);
    for (int i = 0; i < 32; i++) second[i] = rand();

    CU_ASSERT_EQUAL(memcmp(first, second, sizeof(first)), 0);
}

static void test_rand_differs_between_seeds(void) {
    int a[8], b[8];

    srand(1);
    for (int i = 0; i < 8; i++) a[i] = rand();

    srand(2);
    for (int i = 0; i < 8; i++) b[i] = rand();

    CU_ASSERT_NOT_EQUAL(memcmp(a, b, sizeof(a)), 0);
}

static void test_rand_within_range(void) {
    srand(99);
    int in_range = 1;
    int saw_distinct = 0;
    int prev = rand();
    for (int i = 0; i < 500; i++) {
        int r = rand();
        if (r < 0 || r > RAND_MAX) in_range = 0;
        if (r != prev) saw_distinct = 1;
        prev = r;
    }
    CU_ASSERT_TRUE(in_range);
    CU_ASSERT_TRUE(saw_distinct);      /* not a constant generator */
}

/* ----------------------------------------------------------------- qsort */

static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

static int cmp_int_desc(const void *a, const void *b) {
    return cmp_int(b, a);
}

static int is_sorted(const int *v, size_t n) {
    for (size_t i = 1; i < n; i++) {
        if (v[i - 1] > v[i]) return 0;
    }
    return 1;
}

static void test_qsort_ints(void) {
    int v[] = { 9, 3, 7, 1, 8, 2, 6, 0, 5, 4 };
    qsort(v, 10, sizeof(int), cmp_int);

    for (int i = 0; i < 10; i++) {
        CU_ASSERT_EQUAL(v[i], i);
    }
}

static void test_qsort_descending_comparator(void) {
    int v[] = { 4, 1, 3, 0, 2 };
    qsort(v, 5, sizeof(int), cmp_int_desc);

    for (int i = 0; i < 5; i++) {
        CU_ASSERT_EQUAL(v[i], 4 - i);
    }
}

static void test_qsort_sorted_and_all_equal(void) {
    /* Already sorted, reverse sorted, and all-equal are the inputs that break
     * a naive pivot choice. */
    int asc[16], desc[16], same[16];
    for (int i = 0; i < 16; i++) {
        asc[i] = i;
        desc[i] = 15 - i;
        same[i] = 7;
    }

    qsort(asc,  16, sizeof(int), cmp_int);
    qsort(desc, 16, sizeof(int), cmp_int);
    qsort(same, 16, sizeof(int), cmp_int);

    CU_ASSERT_TRUE(is_sorted(asc, 16));
    CU_ASSERT_TRUE(is_sorted(desc, 16));
    CU_ASSERT_TRUE(is_sorted(same, 16));
    CU_ASSERT_EQUAL(desc[0], 0);
    CU_ASSERT_EQUAL(desc[15], 15);
    CU_ASSERT_EQUAL(same[0], 7);
}

static void test_qsort_degenerate_inputs(void) {
    int one = 42;

    qsort(NULL, 0, sizeof(int), cmp_int);     /* must not fault */
    qsort(&one, 0, sizeof(int), cmp_int);
    qsort(&one, 1, sizeof(int), cmp_int);
    qsort(&one, 1, 0, cmp_int);               /* zero element size */
    qsort(&one, 1, sizeof(int), NULL);        /* null comparator */

    CU_ASSERT_EQUAL(one, 42);
}

typedef struct {
    int  key;
    char pad[9];          /* deliberately not a power of two */
} rec_t;

static int cmp_rec(const void *a, const void *b) {
    return ((const rec_t *)a)->key - ((const rec_t *)b)->key;
}

static void test_qsort_arbitrary_element_size(void) {
    rec_t v[12];
    for (int i = 0; i < 12; i++) {
        v[i].key = (11 - i) * 3;
        memset(v[i].pad, (char)('a' + i), sizeof(v[i].pad));
    }

    qsort(v, 12, sizeof(rec_t), cmp_rec);

    int ok = 1;
    for (int i = 0; i < 12; i++) {
        /* Key order, and the payload must have travelled with its key. */
        if (v[i].key != i * 3) ok = 0;
        if (v[i].pad[0] != (char)('a' + (11 - i))) ok = 0;
    }
    CU_ASSERT_TRUE(ok);
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void test_qsort_strings(void) {
    const char *v[] = { "zeta", "alpha", "mu", "beta", "omega" };
    qsort(v, 5, sizeof(const char *), cmp_str);

    CU_ASSERT_STRING_EQUAL(v[0], "alpha");
    CU_ASSERT_STRING_EQUAL(v[1], "beta");
    CU_ASSERT_STRING_EQUAL(v[2], "mu");
    CU_ASSERT_STRING_EQUAL(v[3], "omega");
    CU_ASSERT_STRING_EQUAL(v[4], "zeta");
}

static void test_qsort_large_heap_array(void) {
    /* Big enough to drive the recursion rather than the insertion-sort base
     * case, and heap-allocated so it does not sit on the 16 KiB kernel stack. */
    enum { N = 512 };
    int *v = (int *)malloc(N * sizeof(int));
    CU_ASSERT_PTR_NOT_NULL(v);
    if (v == NULL) return;

    srand(2024);
    for (int i = 0; i < N; i++) v[i] = rand();

    qsort(v, N, sizeof(int), cmp_int);
    CU_ASSERT_TRUE(is_sorted(v, N));

    free(v);
}

/* ------------------------------------------------- qsort: equal-key cost */

/*
 * SCRUM-171.  The two-way Hoare partition this replaced scanned with
 * `cmp(i, lo) <= 0` / `cmp(j, lo) >= 0`, so both pointers ran straight over
 * keys equal to the pivot and every partition on an input with few distinct
 * values split n-1/0 -- O(n^2).  Measured at n = 200,000 before the fix:
 * all-equal 2.0e10 comparisons (5882x n log n, 29 s), two distinct values
 * 1.5e10 (4431x, 18 s), ten distinct 1.7e9 (501x, 2.2 s).
 *
 * These tests assert *comparison counts*, not wall time.  The count is a
 * property of the algorithm and so is identical on the host and under QEMU,
 * where a timing bound would just be a flake generator.
 */
static unsigned long qsort_cmp_count;

static int cmp_int_counting(const void *a, const void *b) {
    qsort_cmp_count++;
    return cmp_int(a, b);
}

typedef struct {
    int      key;
    uint32_t tag;
} keyed_t;

static int cmp_keyed(const void *a, const void *b) {
    int x = ((const keyed_t *)a)->key, y = ((const keyed_t *)b)->key;
    return (x > y) - (x < y);
}

/*
 * The three-way partition moves whole equal-key runs with block swaps, so a
 * wrong run length would drop or duplicate elements rather than merely
 * mis-order them -- which a sortedness check alone would not catch.  Every
 * element carries a unique tag; all N tags must come back exactly once.
 */
static void test_qsort_duplicate_keys_permute(void) {
    enum { N = 4096 };
    static const int distinct[] = { 1, 2, 3, 10, 64 };

    keyed_t       *v    = (keyed_t *)malloc(N * sizeof(keyed_t));
    unsigned char *seen = (unsigned char *)malloc(N);
    CU_ASSERT_PTR_NOT_NULL(v);
    CU_ASSERT_PTR_NOT_NULL(seen);
    if (v == NULL || seen == NULL) {
        free(v);
        free(seen);
        return;
    }

    for (size_t d = 0; d < sizeof(distinct) / sizeof(distinct[0]); d++) {
        srand(7 + (unsigned)d);
        for (int i = 0; i < N; i++) {
            v[i].key = rand() % distinct[d];
            v[i].tag = (uint32_t)i;
        }
        memset(seen, 0, N);

        qsort(v, N, sizeof(keyed_t), cmp_keyed);

        int ordered = 1, permuted = 1;
        for (int i = 0; i < N; i++) {
            if (i > 0 && v[i - 1].key > v[i].key) {
                ordered = 0;
            }
            if (v[i].tag >= (uint32_t)N || seen[v[i].tag]) {
                permuted = 0;
            } else {
                seen[v[i].tag] = 1;
            }
        }
        CU_ASSERT_TRUE(ordered);
        CU_ASSERT_TRUE(permuted);
    }

    free(v);
    free(seen);
}

/*
 * The regression guard itself: an input with few distinct values must cost
 * O(n log n) comparisons, not O(n^2).  Budgets are stated as a multiple of N
 * and sit ~5x above what the current implementation spends and >=12x below
 * what the old partition spent at this N, so a genuine regression fails
 * loudly while ordinary pivot tuning does not.
 */
static void test_qsort_equal_keys_stay_subquadratic(void) {
    enum { N = 4096 };
    static const struct {
        int      distinct;
        unsigned budget_x;   /* comparisons allowed, as a multiple of N */
    } shapes[] = {
        {  1,  4 },     /* all-equal: 4097 measured -- a single O(n) pass */
        {  2,  8 },     /* two distinct:  6158 measured                   */
        { 10, 16 },     /* ten distinct: 12382 measured                   */
    };

    int *v = (int *)malloc(N * sizeof(int));
    CU_ASSERT_PTR_NOT_NULL(v);
    if (v == NULL) {
        return;
    }

    for (size_t s = 0; s < sizeof(shapes) / sizeof(shapes[0]); s++) {
        srand(1);
        for (int i = 0; i < N; i++) {
            v[i] = rand() % shapes[s].distinct;
        }

        qsort_cmp_count = 0;
        qsort(v, N, sizeof(int), cmp_int_counting);

        CU_ASSERT_TRUE(is_sorted(v, N));
        /* The old partition spent N*N/2 here -- 2048x N for all-equal. */
        CU_ASSERT_TRUE(qsort_cmp_count <= (unsigned long)shapes[s].budget_x * N);
    }

    free(v);
}

/*
 * Duplicates must not cost the distinct-key shapes anything either: sorted,
 * reverse and random input are all still bounded by a small multiple of
 * n log2 n (measured 0.82x, 0.82x and 1.04x respectively at this N).
 */
static void test_qsort_distinct_keys_stay_n_log_n(void) {
    enum { N = 4096, LOG2_N = 12 };
    const unsigned long budget = 4UL * N * LOG2_N;

    int *v = (int *)malloc(N * sizeof(int));
    CU_ASSERT_PTR_NOT_NULL(v);
    if (v == NULL) {
        return;
    }

    for (int shape = 0; shape < 3; shape++) {
        srand(99);
        for (int i = 0; i < N; i++) {
            v[i] = (shape == 0) ? i : (shape == 1) ? (N - 1 - i) : rand();
        }

        qsort_cmp_count = 0;
        qsort(v, N, sizeof(int), cmp_int_counting);

        CU_ASSERT_TRUE(is_sorted(v, N));
        CU_ASSERT_TRUE(qsort_cmp_count <= budget);
    }

    free(v);
}

void suite_stdlib_tests(CU_pSuite s) {
    CU_add_test(s, "malloc nonnull and aligned",      test_malloc_nonnull_and_aligned);
    CU_add_test(s, "malloc routes to kernel heap",    test_malloc_routes_to_kernel_heap);
    CU_add_test(s, "malloc blocks don't overlap",     test_malloc_blocks_dont_overlap);
    CU_add_test(s, "free(NULL) is a no-op",           test_free_null_is_noop);
    CU_add_test(s, "realloc preserves data",          test_realloc_preserves_data);
    CU_add_test(s, "realloc NULL and zero",           test_realloc_null_and_zero);
    CU_add_test(s, "atoi basic",                      test_atoi_basic);
    CU_add_test(s, "atoi sign and whitespace",        test_atoi_sign_and_whitespace);
    CU_add_test(s, "atoi stops at garbage",           test_atoi_stops_at_garbage);
    CU_add_test(s, "atoi saturates on overflow",      test_atoi_saturates_on_overflow);
    CU_add_test(s, "abs",                             test_abs);
    CU_add_test(s, "rand seed 1 reference vector",    test_rand_seed_1_reference_vector);
    CU_add_test(s, "rand deterministic per seed",     test_rand_is_deterministic_per_seed);
    CU_add_test(s, "rand differs between seeds",      test_rand_differs_between_seeds);
    CU_add_test(s, "rand within range",               test_rand_within_range);
    CU_add_test(s, "qsort ints",                      test_qsort_ints);
    CU_add_test(s, "qsort descending comparator",     test_qsort_descending_comparator);
    CU_add_test(s, "qsort sorted and all-equal",      test_qsort_sorted_and_all_equal);
    CU_add_test(s, "qsort degenerate inputs",         test_qsort_degenerate_inputs);
    CU_add_test(s, "qsort arbitrary element size",    test_qsort_arbitrary_element_size);
    CU_add_test(s, "qsort strings",                   test_qsort_strings);
    CU_add_test(s, "qsort large heap array",          test_qsort_large_heap_array);
    CU_add_test(s, "qsort duplicate keys permute",     test_qsort_duplicate_keys_permute);
    CU_add_test(s, "qsort equal keys stay subquadratic", test_qsort_equal_keys_stay_subquadratic);
    CU_add_test(s, "qsort distinct keys stay n log n", test_qsort_distinct_keys_stay_n_log_n);
}
