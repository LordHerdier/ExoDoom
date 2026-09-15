/*
 * test_heap_stress_k.c -- kernel heap stress and leak audit (SCRUM-27).
 *
 * Drives src/heap.c with 10,000 blocks in varied sizes and proves the heap
 * comes back to exactly where it started.  SCRUM-26 (on-demand growth) is the
 * prerequisite: without it the run would stop at the initial heap's ceiling
 * and the test would be measuring the wrong failure.
 *
 * Two shapes of load, because they break different things:
 *
 *   churn -- 10,000 alloc/free cycles over a small rolling working set.  Every
 *            free has live neighbours on both sides, so this is what exercises
 *            split/coalesce and the free-list bookkeeping hardest.
 *   peak  -- 10,000 blocks live *simultaneously*, then all freed.  This is
 *            what forces repeated heap_grow_one_page() and, on the way back
 *            down, proves that coalescing actually reassembles the heap
 *            rather than leaving it in fragments.
 *
 * Both shapes run twice from the same PRNG seed, so the second pass replays
 * the first byte for byte.  That is what makes the acceptance criterion
 * checkable: the first pass is a warm-up that lets the heap grow to its
 * high-water mark, and the second must then run entirely inside that space --
 * same free bytes at the end as at the start, and not one new page taken from
 * the PMM.  Comparing before/after across the *first* pass would only measure
 * growth, which is not a leak.
 *
 * Leak detection keys on used_bytes/used_blocks rather than free_bytes:
 * those are the counters that survive the heap growing underneath them (see
 * heap.h).  free_bytes is the stronger statement and is checked too, but only
 * between the two fully-freed, equally-warm states where it is meaningful.
 */

#include "kunit.h"
#include "heap.h"
#include "stdio.h"

#include <stddef.h>
#include <stdint.h>

/* ---- Load parameters ---------------------------------------------------- */

#define STRESS_BLOCKS  10000u   /* blocks held at once in the peak phase     */
#define CHURN_OPS      10000u   /* alloc/free cycles in the churn phase      */
#define CHURN_LIVE       128u   /* rolling working set during churn          */
#define STRESS_SEED  0x5CC2001Bu

/* Free order for the peak phase.  Coprime with STRESS_BLOCKS, so stepping by
 * it visits all 10,000 indices exactly once -- a scattered teardown that makes
 * coalescing merge forwards, backwards and into both neighbours at once,
 * which freeing in allocation order never would. */
#define FREE_STRIDE     4801u

/*
 * Largest request the size mix can produce.  Deliberately under the 4032
 * bytes a freshly created single-page segment has to offer (4096 less
 * sizeof(segment_t) and sizeof(block_t)): a request bigger than that cannot
 * be served by a segment created to satisfy it, so if alloc_page() ever
 * handed back non-contiguous pages the growth loop in heap_alloc() would keep
 * taking pages until the PMM ran dry.  docs/memory.md sec6b calls this the
 * stranded-segment case.  Multi-page allocations are covered by
 * test_heap_k.c's "large alloc forces growth"; this file's job is volume, so
 * it stays on the safe side of that edge instead of racing it 10,000 times.
 */
#define STRESS_MAX_SIZE  3719u

/* ---- Deterministic PRNG -------------------------------------------------
 *
 * xorshift32, local to this file rather than stdlib's rand().  Two reasons:
 * the stdlib suite pins rand()'s seeded sequence, so sharing the generator
 * would couple that test's outcome to suite registration order; and replaying
 * a pass exactly requires a generator nothing else can perturb.
 */

static uint32_t rng_state;

static void rng_seed(uint32_t seed)
{
    rng_state = seed != 0u ? seed : 1u;
}

static uint32_t rng_next(void)
{
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

/* Varied sizes, weighted towards the small end the way a real allocator load
 * is.  Class and offset come from different halves of one 32-bit draw so the
 * two stay independent.  Mean is roughly 390 bytes, which puts the peak phase
 * at about 4 MB across ~1,000 pages -- well inside the ~242 MB the PMM
 * manages under QEMU -m 256M (docs/memory.md sec6). */
static size_t pick_size(void)
{
    uint32_t r   = rng_next();
    uint32_t cls = (r >> 16) % 20u;
    uint32_t off = r & 0xFFFFu;

    if (cls < 12u) return (size_t)(   8u + off %  128u);  /* 60%:    8..135  */
    if (cls < 17u) return (size_t)( 136u + off %  512u);  /* 25%:  136..647  */
    if (cls < 19u) return (size_t)( 648u + off % 1024u);  /* 10%:  648..1671 */
    return             (size_t)(1672u + off % 2048u);     /*  5%: 1672..3719 */
}

/* ---- Payload canaries ---------------------------------------------------
 *
 * Two bytes per block, at the first and last payload byte, keyed to a tag
 * unique per allocation.  Overlapping blocks collide at a boundary in all but
 * contrived cases, and an O(1) stamp is what keeps 10,000 blocks affordable
 * inside CI's 30-second QEMU budget.  The minimum size class is 8, so the two
 * bytes are never the same byte.
 */

static unsigned char head_byte(uint32_t tag) { return (unsigned char)(tag * 31u + 7u); }
static unsigned char tail_byte(uint32_t tag) { return (unsigned char)~(tag * 31u + 7u); }

static void stamp(unsigned char *p, size_t size, uint32_t tag)
{
    p[0]        = head_byte(tag);
    p[size - 1] = tail_byte(tag);
}

static int stamp_ok(const unsigned char *p, size_t size, uint32_t tag)
{
    return p[0] == head_byte(tag) && p[size - 1] == tail_byte(tag);
}

/* ---- Phase results ------------------------------------------------------
 *
 * A phase stops at its first fault and reports it, instead of asserting in
 * place: a genuinely broken allocator would otherwise emit thousands of
 * identical failures and bury the one that matters.
 */

typedef struct {
    int         ok;
    const char *what;
    uint32_t    at;     /* op number (churn) or block index (peak) */
    size_t      size;   /* the request in play when it went wrong  */
} phase_result_t;

static const phase_result_t PHASE_OK = { 1, NULL, 0u, 0u };

static void report_phase(const char *phase, const phase_result_t *r)
{
    if (r->ok) {
        return;
    }
    printf("heap-stress: %s FAILED -- %s (at=%u size=%u)\n",
           phase, r->what, (unsigned)r->at, (unsigned)r->size);
}

/* ---- Phase 1: churn over a rolling working set --------------------------- */

static phase_result_t churn_pass(void)
{
    void     *live[CHURN_LIVE];
    uint16_t  lsz [CHURN_LIVE];
    uint32_t  ltag[CHURN_LIVE];
    phase_result_t r = PHASE_OK;
    uint32_t op, i;

    for (i = 0u; i < CHURN_LIVE; i++) {
        live[i] = NULL;
        lsz[i]  = 0u;
        ltag[i] = 0u;
    }

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
            heap_free(live[slot]);
            live[slot] = NULL;
        }

        sz = pick_size();
        p  = (unsigned char *)heap_alloc(sz);
        if (p == NULL) {
            r.ok = 0; r.what = "heap_alloc returned NULL"; r.at = op; r.size = sz;
            goto drain;
        }
        if (((uintptr_t)p % 16u) != 0u) {
            r.ok = 0; r.what = "pointer lost 16-byte alignment";
            r.at = op; r.size = sz;
            heap_free(p);
            goto drain;
        }

        stamp(p, sz, op);
        live[slot] = p;
        lsz[slot]  = (uint16_t)sz;
        ltag[slot] = op;
    }

drain:
    /* Always hand the whole working set back, failure or not -- a phase that
     * bailed early must not leave the next one measuring its debris. */
    for (i = 0u; i < CHURN_LIVE; i++) {
        if (live[i] != NULL) {
            heap_free(live[i]);
            live[i] = NULL;
        }
    }
    return r;
}

/* ---- Phase 2: 10,000 blocks live at once --------------------------------- */

/* .bss, not stack: 10,000 pointers plus their sizes is ~100 KB and the kernel
 * stack is 16 KiB (see CLAUDE.md). */
static void     *g_blocks[STRESS_BLOCKS];
static uint16_t  g_sizes [STRESS_BLOCKS];

static phase_result_t peak_pass(void)
{
    phase_result_t r = PHASE_OK;
    uint32_t i, idx;

    for (i = 0u; i < STRESS_BLOCKS; i++) {
        g_blocks[i] = NULL;
        g_sizes[i]  = 0u;
    }

    for (i = 0u; i < STRESS_BLOCKS; i++) {
        size_t sz = pick_size();
        unsigned char *p = (unsigned char *)heap_alloc(sz);

        if (p == NULL) {
            r.ok = 0; r.what = "heap_alloc returned NULL"; r.at = i; r.size = sz;
            break;
        }
        if (((uintptr_t)p % 16u) != 0u) {
            r.ok = 0; r.what = "pointer lost 16-byte alignment";
            r.at = i; r.size = sz;
            heap_free(p);
            break;
        }

        stamp(p, sz, i);
        g_blocks[i] = p;
        g_sizes[i]  = (uint16_t)sz;
    }

    /* With all 10,000 still held, re-read every canary.  If any two live
     * blocks overlapped, one of them has had a byte overwritten by the
     * other's stamp -- the check that first-fit and split_block never hand
     * the same bytes out twice. */
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
            heap_free(g_blocks[idx]);
            g_blocks[idx] = NULL;
        }
        idx = (idx + FREE_STRIDE) % STRESS_BLOCKS;
    }
    return r;
}

/* ---- Suite driver -------------------------------------------------------
 *
 * The load runs once, in suite init, and the tests below read the results.
 * Doing it here rather than inside the first test keeps each test a single
 * independent claim about the same run, and puts the serial snapshots in one
 * uninterrupted block.
 */

static heap_stats_t   s_entry, s_warm, s_measured;
static phase_result_t r_warm_churn, r_warm_peak;
static phase_result_t r_meas_churn, r_meas_peak;

int heap_stress_suite_init(void)
{
    printf("\nheap-stress: 2 passes x (%u churn ops over %u slots + "
           "%u live blocks, 8..%u bytes)\n",
           (unsigned)CHURN_OPS, (unsigned)CHURN_LIVE,
           (unsigned)STRESS_BLOCKS, (unsigned)STRESS_MAX_SIZE);

    heap_get_stats(&s_entry);
    heap_report("before");

    /* Pass 1 -- warm-up.  Lets the heap grow to its high-water mark, so the
     * pass that gets measured is the one that should need no new memory. */
    rng_seed(STRESS_SEED);
    r_warm_churn = churn_pass();
    r_warm_peak  = peak_pass();
    heap_get_stats(&s_warm);
    heap_report("after warm-up");

    /* Pass 2 -- measured.  Same seed, so byte-for-byte the same work. */
    rng_seed(STRESS_SEED);
    r_meas_churn = churn_pass();
    r_meas_peak  = peak_pass();
    heap_get_stats(&s_measured);
    heap_report("after");

    printf("heap-stress: free delta across measured pass = %d bytes, "
           "pages taken = %u\n",
           (int)((long)s_measured.free_bytes - (long)s_warm.free_bytes),
           (unsigned)(s_measured.pages - s_warm.pages));

    /* Always 0: a phase that failed is reported by the tests below, which say
     * far more than KUnit's "suite init failed, skipping" would. */
    return 0;
}

/* ---- Tests --------------------------------------------------------------- */

static void test_warmup_pass_completes(void)
{
    report_phase("warm-up churn", &r_warm_churn);
    report_phase("warm-up peak",  &r_warm_peak);
    CU_ASSERT_TRUE(r_warm_churn.ok);
    CU_ASSERT_TRUE(r_warm_peak.ok);
}

static void test_measured_pass_completes(void)
{
    report_phase("measured churn", &r_meas_churn);
    report_phase("measured peak",  &r_meas_peak);
    CU_ASSERT_TRUE(r_meas_churn.ok);
    CU_ASSERT_TRUE(r_meas_peak.ok);
}

/* The leak check proper.  Growth-independent: every block either phase took
 * was handed back, so the busy counters must read exactly what they did
 * before any of it started. */
static void test_no_leak_used_bytes_conserved(void)
{
    CU_ASSERT_EQUAL(s_measured.used_bytes,  s_entry.used_bytes);
    CU_ASSERT_EQUAL(s_measured.used_blocks, s_entry.used_blocks);
    CU_ASSERT_EQUAL(s_warm.used_bytes,      s_entry.used_bytes);
    CU_ASSERT_EQUAL(s_warm.used_blocks,     s_entry.used_blocks);
}

/* SCRUM-27's acceptance criterion: free memory before the measured pass and
 * free memory after it are the same number, reported on serial above. */
static void test_free_memory_consistent_across_pass(void)
{
    CU_ASSERT_EQUAL(s_measured.free_bytes,  s_warm.free_bytes);
    CU_ASSERT_EQUAL(s_measured.total_bytes, s_warm.total_bytes);
}

/* Coalescing has to actually reassemble the heap, not just balance the byte
 * count: an identical free-block population is what says 20,000 allocations
 * left no fragmentation behind. */
static void test_free_list_fully_reassembled(void)
{
    CU_ASSERT_EQUAL(s_measured.free_blocks, s_warm.free_blocks);
    CU_ASSERT_EQUAL(s_measured.segments,    s_warm.segments);
}

/* The warm-up already paid for every page the load needs, so the replay must
 * fit inside them -- if it doesn't, free space is being lost somewhere the
 * byte counters alone would not catch. */
static void test_measured_pass_takes_no_new_pages(void)
{
    CU_ASSERT_EQUAL(s_measured.pages, s_warm.pages);
}

/* Sanity on the fixture itself: a stress test that quietly stopped growing
 * the heap would pass everything above while proving nothing. */
static void test_load_actually_grew_the_heap(void)
{
    CU_ASSERT_TRUE(s_warm.pages > s_entry.pages);
    CU_ASSERT_TRUE(s_warm.total_bytes > s_entry.total_bytes);
}

void suite_heap_stress_tests(CU_pSuite s)
{
    CU_add_test(s, "warm-up pass: 10K churn ops + 10K live blocks",
                test_warmup_pass_completes);
    CU_add_test(s, "measured pass: 10K churn ops + 10K live blocks",
                test_measured_pass_completes);
    CU_add_test(s, "no leak: used bytes and blocks back to baseline",
                test_no_leak_used_bytes_conserved);
    CU_add_test(s, "free memory consistent before and after",
                test_free_memory_consistent_across_pass);
    CU_add_test(s, "free list fully reassembled by coalescing",
                test_free_list_fully_reassembled);
    CU_add_test(s, "measured pass takes no new pages from the PMM",
                test_measured_pass_takes_no_new_pages);
    CU_add_test(s, "the load did grow the heap (fixture sanity)",
                test_load_actually_grew_the_heap);
}
