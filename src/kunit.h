/*
 * kunit.h — Bare-metal CUnit-compatible test framework for ExoDoom.
 *
 * Mirrors the CUnit 2.x public API so tests can be written in standard CUnit
 * style with no libc dependency.  Output goes to the COM1 serial port via
 * serial_print / serial_putc.
 *
 * Include this header in test files.  Compile the kernel with -DTESTING to
 * activate the test runner path in kernel_main.
 */

#ifndef KUNIT_H
#define KUNIT_H

#include "string.h"   /* strcmp for CU_ASSERT_STRING_* macros */

/* ---- Capacity limits --------------------------------------------------- */
/* Increase these if you hit the suite or per-suite test ceiling.
 *
 * Note the ceiling is NOT reported at link time, and not at run time either:
 * CU_add_suite returns NULL once it is reached, CU_add_test(NULL, ...) is a
 * no-op, and the run still prints ALL TESTS PASSED with the whole suite
 * silently missing.  Raised from 16 to 32 when the fault suite (SCRUM-17)
 * took the last slot, and from 32 to 48 under SCRUM-107: the 32 cap had
 * already been silently exceeded on main by the time this ticket started --
 * 36 suites were registered in tests/kernel/test_runner.c against a 32 slot
 * table, so libos_heap_stress, port_io_fault, kernel_mem_fault and
 * irq_entry (the four most recently added, landing after the fault suite
 * took slot 32) were never actually running, with "ALL TESTS PASSED" still
 * printing because that is exactly the silent failure mode this comment
 * already warned about. Discovered because the new context suite (37th)
 * hit it too. Raised again from 48 to 64 under SCRUM-73: main had reached
 * 48 registered suites on its own, so that branch's dg_init suite was the
 * 49th and would have been silently dropped -- CU_add_suite returning NULL,
 * the suite running zero tests, and ALL TESTS PASSED printing anyway, which
 * is the exact failure this comment has now warned about twice. SCRUM-65
 * needed the same raise independently and for the same reason, which is
 * why the two branches made an identical change: a ceiling whose overflow
 * mode is silence leaves no safe margin, so do not trim this back toward
 * the current count. Raised from 64 to 256 under SCRUM-185, well ahead of
 * the 51 suites registered in tests/kernel/test_runner.c at the time, to
 * stop this needing a bump every few sprints. */
#define KUNIT_MAX_SUITES          256

/* The per-suite ceiling has exactly the same silent-overflow mode as
 * KUNIT_MAX_SUITES above, and bit for the same reason: CU_add_test() returns
 * NULL past the cap, test_runner.c does not check the return (nor should it
 * have to), and the dropped tests simply never run while ALL TESTS PASSED
 * still prints. Raised from 64 to 128 under SCRUM-189, whose exofs suite
 * reached 66 tests and silently lost its last two -- including the ticket's
 * own end-to-end acceptance test, which is the single test most worth
 * noticing the absence of.
 *
 * The cost is real but small: the registry is a flat static array, so this
 * is KUNIT_MAX_SUITES * KUNIT_MAX_TESTS_PER_SUITE * sizeof(CU_Test) of .bss
 * in a TESTING build, and doubling the per-suite cap doubles it. Do not trim
 * either constant back toward the current count -- a ceiling whose overflow
 * mode is silence leaves no safe margin.
 *
 * If a suite ever genuinely approaches 128, split it rather than raising
 * this again: test_libos_heap_k.c's two-suite split shows the shape, and a
 * suite that large has usually stopped being one subject. */
#define KUNIT_MAX_TESTS_PER_SUITE 128
#define KUNIT_NAME_LEN            64

/* ---- Opaque handle types ----------------------------------------------- */
typedef struct _CU_Suite  CU_Suite;
typedef struct _CU_Test   CU_Test;
typedef CU_Suite         *CU_pSuite;
typedef CU_Test          *CU_pTest;

/* ---- Callback signatures ----------------------------------------------- */
typedef void (*CU_TestFunc)(void);
typedef int  (*CU_InitializeFunc)(void);
typedef int  (*CU_CleanupFunc)(void);

/* ---- Error codes (CUnit subset) ---------------------------------------- */
typedef enum {
    CUE_SUCCESS           = 0,
    CUE_NOMEMORY          = 1,
    CUE_NOREGISTRY        = 2,
    CUE_SUITE_INIT_FAILED = 3,
} CU_ErrorCode;

/* ---- Registry lifecycle ------------------------------------------------ */
void         CU_initialize_registry(void);
CU_ErrorCode CU_get_error(void);

/* ---- Registration ------------------------------------------------------ */
CU_pSuite CU_add_suite(const char *name,
                       CU_InitializeFunc init,
                       CU_CleanupFunc cleanup);
CU_pTest  CU_add_test(CU_pSuite suite,
                      const char *name,
                      CU_TestFunc func);

/* ---- Execution --------------------------------------------------------- */
CU_ErrorCode CU_run_all_tests(void);

/* ---- Result accessors -------------------------------------------------- */
unsigned CU_get_number_of_tests_run(void);
unsigned CU_get_number_of_tests_failed(void);
unsigned CU_get_number_of_assertions(void);
unsigned CU_get_number_of_assertion_failures(void);

/* ---- Internal: do not call directly ------------------------------------ */
void _kunit_assert(int pass, const char *expr,
                   const char *file, int line);

/* ---- Assert macros ----------------------------------------------------- */
#define CU_ASSERT(expr) \
    _kunit_assert(!!(expr), #expr, __FILE__, __LINE__)

#define CU_ASSERT_TRUE(expr)  CU_ASSERT(expr)
#define CU_ASSERT_FALSE(expr) CU_ASSERT(!(expr))

#define CU_ASSERT_EQUAL(actual, expected) \
    _kunit_assert((actual) == (expected), \
                  #actual " == " #expected, __FILE__, __LINE__)

#define CU_ASSERT_NOT_EQUAL(actual, expected) \
    _kunit_assert((actual) != (expected), \
                  #actual " != " #expected, __FILE__, __LINE__)

#define CU_ASSERT_PTR_NULL(ptr) \
    _kunit_assert((ptr) == (void *)0, \
                  #ptr " == NULL", __FILE__, __LINE__)

#define CU_ASSERT_PTR_NOT_NULL(ptr) \
    _kunit_assert((ptr) != (void *)0, \
                  #ptr " != NULL", __FILE__, __LINE__)

#define CU_ASSERT_STRING_EQUAL(actual, expected) \
    _kunit_assert(strcmp((actual), (expected)) == 0, \
                  #actual " str== " #expected, __FILE__, __LINE__)

#define CU_ASSERT_STRING_NOT_EQUAL(actual, expected) \
    _kunit_assert(strcmp((actual), (expected)) != 0, \
                  #actual " str!= " #expected, __FILE__, __LINE__)

#endif /* KUNIT_H */
