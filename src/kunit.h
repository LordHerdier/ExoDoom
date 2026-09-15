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
 * hit it too. */
#define KUNIT_MAX_SUITES          48
#define KUNIT_MAX_TESTS_PER_SUITE 64
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
