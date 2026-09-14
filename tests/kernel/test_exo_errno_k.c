/*
 * test_exo_errno_k.c — exo_errno.h error-code contract (SCRUM-57).
 *
 * Acceptance criteria under test:
 *   - every EXO_E* code matches the numeric value of the unprefixed libc
 *     errno.h name of the same meaning (docs/syscall_spec.md §3.2a) — so a
 *     future libc shim can pass one straight through as `errno`
 *   - no two EXO_E* codes alias the same value
 *
 * Both headers define plain macros rather than enums, so "matches" has to be
 * checked value-by-value rather than by comparing types.
 */

#include "kunit.h"
#include "exo_syscall.h"
#include "errno.h"

/* ---- Parity with src/errno.h -------------------------------------------- */

static void test_codes_match_libc_errno(void)
{
    CU_ASSERT_EQUAL(EXO_EPERM,    EPERM);
    CU_ASSERT_EQUAL(EXO_ENOENT,   ENOENT);
    CU_ASSERT_EQUAL(EXO_EBADF,    EBADF);
    CU_ASSERT_EQUAL(EXO_ENOMEM,   ENOMEM);
    CU_ASSERT_EQUAL(EXO_EACCES,   EACCES);
    CU_ASSERT_EQUAL(EXO_EBUSY,    EBUSY);
    CU_ASSERT_EQUAL(EXO_EEXIST,   EEXIST);
    CU_ASSERT_EQUAL(EXO_ENODEV,   ENODEV);
    CU_ASSERT_EQUAL(EXO_ENOTDIR,  ENOTDIR);
    CU_ASSERT_EQUAL(EXO_EISDIR,   EISDIR);
    CU_ASSERT_EQUAL(EXO_EINVAL,   EINVAL);
    CU_ASSERT_EQUAL(EXO_ENFILE,   ENFILE);
    CU_ASSERT_EQUAL(EXO_EMFILE,   EMFILE);
    CU_ASSERT_EQUAL(EXO_EFBIG,    EFBIG);
    CU_ASSERT_EQUAL(EXO_ENOSPC,   ENOSPC);
    CU_ASSERT_EQUAL(EXO_ESPIPE,   ESPIPE);
    CU_ASSERT_EQUAL(EXO_EROFS,    EROFS);
    CU_ASSERT_EQUAL(EXO_ENOSYS,   ENOSYS);

    /* EXO_EFAULT has no libc errno.h counterpart -- there is no pointer
     * argument to a libc call that crosses the LibOS/kernel boundary the way
     * a syscall argument does, so nothing in errno.h needed the concept.
     * Pinned against Linux's EFAULT value directly instead. */
    CU_ASSERT_EQUAL(EXO_EFAULT, 14);
}

/* ---- No two codes alias the same value ---------------------------------- */

static void test_codes_are_unique(void)
{
    static const int codes[] = {
        EXO_EPERM,   EXO_ENOENT,  EXO_EBADF,   EXO_ENOMEM,  EXO_EACCES,
        EXO_EFAULT,  EXO_EBUSY,   EXO_EEXIST,  EXO_ENODEV,  EXO_ENOTDIR,
        EXO_EISDIR,  EXO_EINVAL,  EXO_ENFILE,  EXO_EMFILE,  EXO_EFBIG,
        EXO_ENOSPC,  EXO_ESPIPE,  EXO_EROFS,   EXO_ENOSYS,
    };
    unsigned i, j;
    const unsigned n = sizeof(codes) / sizeof(codes[0]);

    for (i = 0; i < n; i++) {
        CU_ASSERT_TRUE(codes[i] > 0);
        for (j = i + 1; j < n; j++) {
            CU_ASSERT_TRUE(codes[i] != codes[j]);
        }
    }
}

void suite_exo_errno_tests(CU_pSuite s)
{
    CU_add_test(s, "codes_match_libc_errno", test_codes_match_libc_errno);
    CU_add_test(s, "codes_unique",           test_codes_are_unique);
}
