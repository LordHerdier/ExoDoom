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
#define KUNIT_MAX_SUITES          16
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
