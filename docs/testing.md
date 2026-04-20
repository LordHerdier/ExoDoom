# Testing

ExoDoom uses **KUnit** — a bare-metal CUnit-compatible test framework — to run
unit tests directly inside the kernel at boot time.  Tests execute on real
hardware (or QEMU), so there is no host/target divergence: the same code that
ships runs the tests.

---

## Architecture

```
src/kunit.h              Public CUnit-compatible API
tests/kernel/kunit.c     Framework implementation (serial output, no libc)
tests/kernel/test_runner.c   Suite registration + run_tests() entry point
tests/kernel/test_smoke.c    Smoke tests (harness self-check)
tests/kernel/test_string_k.c String function tests
tests/kernel/test_ctype_k.c  Ctype function tests
```

When the kernel is compiled with `-DTESTING`, `kernel_main` calls
`run_tests()` instead of its normal boot path.  `run_tests()` registers all
suites, calls `CU_run_all_tests()`, and exits QEMU with code 0 on pass or 1
on fail.

---

## Running tests

### In Docker (recommended)

```bash
make docker-test
```

Builds the test kernel with `TESTING=1`, boots it in QEMU, and streams serial
output to your terminal.  Look for `ALL TESTS PASSED` or `TESTS FAILED` at the
end.

### In CI

`make docker-ci` always builds with `TESTING=1`.  The GitHub Actions workflow
greps the serial output for `ALL TESTS PASSED` and fails the job if that
string is absent or if `TESTS FAILED` is present.

---

## Writing tests

### 1. Create a test file in `tests/kernel/`

```c
/* tests/kernel/test_example.c */
#include "kunit.h"
#include "example.h"   /* module under test */

static void test_something(void)
{
    CU_ASSERT_EQUAL(example_add(1, 2), 3);
    CU_ASSERT_STRING_EQUAL(example_name(), "example");
}

void suite_example_tests(CU_pSuite s)
{
    CU_add_test(s, "something", test_something);
}
```

### 2. Register the suite in `tests/kernel/test_runner.c`

```c
extern void suite_example_tests(CU_pSuite s);

int run_tests(void)
{
    /* ... existing suites ... */

    s = CU_add_suite("example", NULL, NULL);
    suite_example_tests(s);

    CU_run_all_tests();
    return CU_get_number_of_tests_failed() != 0 ? 1 : 0;
}
```

No Makefile changes are needed — `build.sh` compiles all `tests/kernel/*.c`
automatically when `TESTING=1`.

---

## Available assert macros

| Macro | Passes when |
|-------|-------------|
| `CU_ASSERT(expr)` | `expr` is non-zero |
| `CU_ASSERT_TRUE(expr)` | `expr` is non-zero |
| `CU_ASSERT_FALSE(expr)` | `expr` is zero |
| `CU_ASSERT_EQUAL(a, b)` | `a == b` |
| `CU_ASSERT_NOT_EQUAL(a, b)` | `a != b` |
| `CU_ASSERT_PTR_NULL(p)` | `p == NULL` |
| `CU_ASSERT_PTR_NOT_NULL(p)` | `p != NULL` |
| `CU_ASSERT_STRING_EQUAL(a, b)` | `strcmp(a, b) == 0` |
| `CU_ASSERT_STRING_NOT_EQUAL(a, b)` | `strcmp(a, b) != 0` |

On failure each macro prints:

```
    ASSERT FAILED: <expr> (<file>:<line>)
```

to the COM1 serial port.

---

## Serial output format

```
Kernel Booted

=== KUnit Test Runner ===

Suite: smoke
  [PASS] assert_true_false
  [PASS] assert_equal
  ...

Suite: string
  [PASS] strlen_basic
  [FAIL] strcmp_order
    ASSERT FAILED: strcmp("abc", "abd") < 0 (tests/kernel/test_string_k.c:42)

=== Summary ===
Suites:     3
Tests run:  22
Assertions: 58
Failures:   1

TESTS FAILED: 1 test(s) failed
```

---

## Framework limits

| Setting | Value |
|---------|-------|
| `KUNIT_MAX_SUITES` | 16 |
| `KUNIT_MAX_TESTS_PER_SUITE` | 64 |
| `KUNIT_NAME_LEN` | 64 bytes |

These can be increased in `src/kunit.h` if needed.
