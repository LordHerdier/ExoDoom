/*
 * test_libos_heap_k.c -- LibOS-side heap allocator (SCRUM-38).
 *
 * Drives src/libos_heap.c directly (libos_heap_alloc/_free/_realloc), which
 * itself drives the real syscall dispatcher through libos_page_alloc()
 * (SCRUM-37) -- see that file's top comment for why via
 * exo_syscall_dispatch() and not the inline `syscall`-instruction stubs.
 *
 * The correctness tests mirror test_heap_k.c's shape exactly (this is the
 * same allocator algorithm, different page source). The stress test at the
 * bottom mirrors test_heap_stress_k.c's churn+peak, warm-up+measured design
 * but at roughly 1/20th the scale: each page grow here costs two real
 * dispatcher round trips (EXO_SYS_PAGE_ALLOC then EXO_SYS_PAGE_MAP) instead
 * of one bitmap-scan alloc_page() call, and docs/testing.md's 30s CI ceiling
 * is shared with every other suite -- see that file before raising these
 * numbers back up.
 */

#include "kunit.h"
#include "libos_heap.h"
#include "libos_page_alloc.h"
#include "stdio.h"

#include <stddef.h>
#include <stdint.h>

/* ---- Direct correctness tests, mirroring test_heap_k.c ------------------ */

static void test_alloc_nonnull_and_aligned(void) {
    void *p = libos_heap_alloc(37);
    CU_ASSERT_PTR_NOT_NULL(p);
    if (p == NULL) return;
    CU_ASSERT_EQUAL((uintptr_t)p % 16, 0);
    libos_heap_free(p);
}

static void test_sequential_allocs_dont_overlap(void) {
    void *a = libos_heap_alloc(64);
    void *b = libos_heap_alloc(128);
    CU_ASSERT_PTR_NOT_NULL(a);
    CU_ASSERT_PTR_NOT_NULL(b);
    if (a == NULL || b == NULL) return;

    uintptr_t ua = (uintptr_t)a, ub = (uintptr_t)b;
    int disjoint = (ua + 64 <= ub) || (ub + 128 <= ua);
    CU_ASSERT_TRUE(disjoint);

    libos_heap_free(a);
    libos_heap_free(b);
}

static void test_free_and_realloc_preserve_data(void) {
    unsigned char *p = (unsigned char *)libos_heap_alloc(32);
    CU_ASSERT_PTR_NOT_NULL(p);
    if (p == NULL) return;
    for (int i = 0; i < 32; i++) p[i] = (unsigned char)i;

    unsigned char *q = (unsigned char *)libos_heap_realloc(p, 256);
    CU_ASSERT_PTR_NOT_NULL(q);
    if (q == NULL) return;
    int ok = 1;
    for (int i = 0; i < 32; i++) {
        if (q[i] != (unsigned char)i) ok = 0;
    }
    CU_ASSERT_TRUE(ok);

    libos_heap_free(q);
}

static void test_free_then_realloc_reuses_block(void) {
    void *a = libos_heap_alloc(64);
    CU_ASSERT_PTR_NOT_NULL(a);
    if (a == NULL) return;
    libos_heap_free(a);

    void *b = libos_heap_alloc(64);
    CU_ASSERT_PTR_NOT_NULL(b);
    CU_ASSERT_EQUAL(a, b);   /* first-fit should reuse the freed block */

    libos_heap_free(b);
}

static void test_large_alloc_forces_growth(void) {
    /* Bigger than one 4K page -- forces heap_grow_one_page() to run more
     * than once and, if contiguous, extend a segment across pages. */
    void *p = libos_heap_alloc(4096 * 3);
    CU_ASSERT_PTR_NOT_NULL(p);
    if (p == NULL) return;

    unsigned char *bytes = (unsigned char *)p;
    bytes[0] = 0xAB;
    bytes[4096 * 3 - 1] = 0xCD;
    CU_ASSERT_EQUAL(bytes[0], 0xAB);
    CU_ASSERT_EQUAL(bytes[4096 * 3 - 1], 0xCD);

    libos_heap_free(p);
}

static void test_realloc_null_and_zero(void) {
    void *p = libos_heap_realloc(NULL, 48);
    CU_ASSERT_PTR_NOT_NULL(p);

    void *q = libos_heap_realloc(p, 0);
    CU_ASSERT_PTR_NULL(q);
}

/* ---- Scaled-down stress + leak audit, mirroring test_heap_stress_k.c ---- */

#define STRESS_BLOCKS  500u
#define CHURN_OPS      500u
#define CHURN_LIVE      32u
#define STRESS_SEED  0x5CC2001Bu
#define FREE_STRIDE     211u   /* coprime with STRESS_BLOCKS */
#define STRESS_MAX_SIZE 3719u

static uint32_t rng_state;

static void rng_seed(uint32_t seed) { rng_state = seed != 0u ? seed : 1u; }

static uint32_t rng_next(void) {
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

static size_t pick_size(void) {
    uint32_t r   = rng_next();
    uint32_t cls = (r >> 16) % 20u;
    uint32_t off = r & 0xFFFFu;

    if (cls < 12u) return (size_t)(   8u + off %  128u);
    if (cls < 17u) return (size_t)( 136u + off %  512u);
    if (cls < 19u) return (size_t)( 648u + off % 1024u);
    return             (size_t)(1672u + off % 2048u);
}

static unsigned char head_byte(uint32_t tag) { return (unsigned char)(tag * 31u + 7u); }
static unsigned char tail_byte(uint32_t tag) { return (unsigned char)~(tag * 31u + 7u); }

static void stamp(unsigned char *p, size_t size, uint32_t tag) {
    p[0]        = head_byte(tag);
    p[size - 1] = tail_byte(tag);
}

static int stamp_ok(const unsigned char *p, size_t size, uint32_t tag) {
    return p[0] == head_byte(tag) && p[size - 1] == tail_byte(tag);
}

typedef struct {
    int         ok;
    const char *what;
    uint32_t    at;
    size_t      size;
} phase_result_t;

static const phase_result_t PHASE_OK = { 1, NULL, 0u, 0u };

static void report_phase(const char *phase, const phase_result_t *r) {
    if (r->ok) return;
    printf("libos-heap-stress: %s FAILED -- %s (at=%u size=%u)\n",
           phase, r->what, (unsigned)r->at, (unsigned)r->size);
}

static phase_result_t churn_pass(void) {
    void     *live[CHURN_LIVE];
    uint16_t  lsz [CHURN_LIVE];
    uint32_t  ltag[CHURN_LIVE];
    phase_result_t r = PHASE_OK;
    uint32_t op, i;

    for (i = 0u; i < CHURN_LIVE; i++) { live[i] = NULL; lsz[i] = 0u; ltag[i] = 0u; }

    for (op = 0u; op < CHURN_OPS; op++) {
        uint32_t slot = rng_next() % CHURN_LIVE;
        unsigned char *p;
        size_t sz;

        if (live[slot] != NULL) {
            if (!stamp_ok((unsigned char *)live[slot], lsz[slot], ltag[slot])) {
                r.ok = 0; r.what = "canary clobbered while block was live";
                r.at = op; r.size = lsz[slot];
                goto drain;
            }
            libos_heap_free(live[slot]);
            live[slot] = NULL;
        }

        sz = pick_size();
        p  = (unsigned char *)libos_heap_alloc(sz);
        if (p == NULL) {
            r.ok = 0; r.what = "libos_heap_alloc returned NULL"; r.at = op; r.size = sz;
            goto drain;
        }
        if (((uintptr_t)p % 16u) != 0u) {
            r.ok = 0; r.what = "pointer lost 16-byte alignment";
            r.at = op; r.size = sz;
            libos_heap_free(p);
            goto drain;
        }

        stamp(p, sz, op);
        live[slot] = p;
        lsz[slot]  = (uint16_t)sz;
        ltag[slot] = op;
    }

drain:
    for (i = 0u; i < CHURN_LIVE; i++) {
        if (live[i] != NULL) {
            libos_heap_free(live[i]);
            live[i] = NULL;
        }
    }
    return r;
}

static void     *g_blocks[STRESS_BLOCKS];
static uint16_t  g_sizes [STRESS_BLOCKS];

static phase_result_t peak_pass(void) {
    phase_result_t r = PHASE_OK;
    uint32_t i, idx;

    for (i = 0u; i < STRESS_BLOCKS; i++) { g_blocks[i] = NULL; g_sizes[i] = 0u; }

    for (i = 0u; i < STRESS_BLOCKS; i++) {
        size_t sz = pick_size();
        unsigned char *p = (unsigned char *)libos_heap_alloc(sz);

        if (p == NULL) {
            r.ok = 0; r.what = "libos_heap_alloc returned NULL"; r.at = i; r.size = sz;
            break;
        }
        if (((uintptr_t)p % 16u) != 0u) {
            r.ok = 0; r.what = "pointer lost 16-byte alignment";
            r.at = i; r.size = sz;
            libos_heap_free(p);
            break;
        }

        stamp(p, sz, i);
        g_blocks[i] = p;
        g_sizes[i]  = (uint16_t)sz;
    }

    if (r.ok) {
        for (i = 0u; i < STRESS_BLOCKS; i++) {
            if (!stamp_ok((unsigned char *)g_blocks[i], g_sizes[i], i)) {
                r.ok = 0; r.what = "two live blocks overlap";
                r.at = i; r.size = g_sizes[i];
                break;
            }
        }
    }

    idx = 0u;
    for (i = 0u; i < STRESS_BLOCKS; i++) {
        if (g_blocks[idx] != NULL) {
            libos_heap_free(g_blocks[idx]);
            g_blocks[idx] = NULL;
        }
        idx = (idx + FREE_STRIDE) % STRESS_BLOCKS;
    }
    return r;
}

static libos_heap_stats_t s_entry, s_warm, s_measured;
static phase_result_t r_warm_churn, r_warm_peak;
static phase_result_t r_meas_churn, r_meas_peak;

int libos_heap_stress_suite_init(void) {
    printf("\nlibos-heap-stress: 2 passes x (%u churn ops over %u slots + "
           "%u live blocks, 8..%u bytes)\n",
           (unsigned)CHURN_OPS, (unsigned)CHURN_LIVE,
           (unsigned)STRESS_BLOCKS, (unsigned)STRESS_MAX_SIZE);

    libos_heap_get_stats(&s_entry);
    libos_heap_report("before");

    rng_seed(STRESS_SEED);
    r_warm_churn = churn_pass();
    r_warm_peak  = peak_pass();
    libos_heap_get_stats(&s_warm);
    libos_heap_report("after warm-up");

    rng_seed(STRESS_SEED);
    r_meas_churn = churn_pass();
    r_meas_peak  = peak_pass();
    libos_heap_get_stats(&s_measured);
    libos_heap_report("after");

    printf("libos-heap-stress: free delta across measured pass = %d bytes, "
           "pages taken = %u\n",
           (int)((long)s_measured.free_bytes - (long)s_warm.free_bytes),
           (unsigned)(s_measured.pages - s_warm.pages));

    return 0;
}

static void test_warmup_pass_completes(void) {
    report_phase("warm-up churn", &r_warm_churn);
    report_phase("warm-up peak",  &r_warm_peak);
    CU_ASSERT_TRUE(r_warm_churn.ok);
    CU_ASSERT_TRUE(r_warm_peak.ok);
}

static void test_measured_pass_completes(void) {
    report_phase("measured churn", &r_meas_churn);
    report_phase("measured peak",  &r_meas_peak);
    CU_ASSERT_TRUE(r_meas_churn.ok);
    CU_ASSERT_TRUE(r_meas_peak.ok);
}

static void test_no_leak_used_bytes_conserved(void) {
    CU_ASSERT_EQUAL(s_measured.used_bytes,  s_entry.used_bytes);
    CU_ASSERT_EQUAL(s_measured.used_blocks, s_entry.used_blocks);
    CU_ASSERT_EQUAL(s_warm.used_bytes,      s_entry.used_bytes);
    CU_ASSERT_EQUAL(s_warm.used_blocks,     s_entry.used_blocks);
}

static void test_free_memory_consistent_across_pass(void) {
    CU_ASSERT_EQUAL(s_measured.free_bytes,  s_warm.free_bytes);
    CU_ASSERT_EQUAL(s_measured.total_bytes, s_warm.total_bytes);
}

static void test_free_list_fully_reassembled(void) {
    CU_ASSERT_EQUAL(s_measured.free_blocks, s_warm.free_blocks);
    CU_ASSERT_EQUAL(s_measured.segments,    s_warm.segments);
}

static void test_measured_pass_takes_no_new_pages(void) {
    CU_ASSERT_EQUAL(s_measured.pages, s_warm.pages);
}

static void test_load_actually_grew_the_heap(void) {
    CU_ASSERT_TRUE(s_warm.pages > s_entry.pages);
    CU_ASSERT_TRUE(s_warm.total_bytes > s_entry.total_bytes);
}

void suite_libos_heap_tests(CU_pSuite s) {
    CU_add_test(s, "alloc nonnull and aligned",       test_alloc_nonnull_and_aligned);
    CU_add_test(s, "sequential allocs don't overlap", test_sequential_allocs_dont_overlap);
    CU_add_test(s, "free and realloc preserve data",  test_free_and_realloc_preserve_data);
    CU_add_test(s, "free then realloc reuses block",  test_free_then_realloc_reuses_block);
    CU_add_test(s, "large alloc forces growth",       test_large_alloc_forces_growth);
    CU_add_test(s, "realloc NULL and zero",           test_realloc_null_and_zero);

    CU_add_test(s, "warm-up pass: 500 churn ops + 500 live blocks",
                test_warmup_pass_completes);
    CU_add_test(s, "measured pass: 500 churn ops + 500 live blocks",
                test_measured_pass_completes);
    CU_add_test(s, "no leak: used bytes and blocks back to baseline",
                test_no_leak_used_bytes_conserved);
    CU_add_test(s, "free memory consistent before and after",
                test_free_memory_consistent_across_pass);
    CU_add_test(s, "free list fully reassembled by coalescing",
                test_free_list_fully_reassembled);
    CU_add_test(s, "measured pass takes no new pages from libos_page_alloc",
                test_measured_pass_takes_no_new_pages);
    CU_add_test(s, "the load did grow the heap (fixture sanity)",
                test_load_actually_grew_the_heap);
}
