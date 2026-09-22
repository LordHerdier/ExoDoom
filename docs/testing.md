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
tests/kernel/test_kbd_ring.c Keyboard event ring + scan code decoder tests
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

### Time budget

Both `docker-test` and `docker-ci` run QEMU under `timeout 180` (raised from
120 in SCRUM-102, which itself raised it from 60 in SCRUM-66 — see below). It
is a hard ceiling, and a suite
that blows through it looks like a *truncated serial log*, not like a
failure: the grep for `ALL TESTS PASSED` simply finds nothing, CI reports the
completion signal as missing, and QEMU's own serial output (already
flushed/buffered up to that point) dumps out right as `timeout` kills the
process — which can look like a pile of failures even when every one of
those lines is an expected rejection from whichever suite was still running.

The expensive suite is `syscall_fuzz` (SCRUM-115): it drives 1,000,000 random
syscalls through the dispatcher. Three of those syscall numbers —
`EXO_SYS_LAUNCH_WAD_VIEWER`/`_CLOCK`/`_DOOM` — do a real `context_create()`
plus a full image copy instead of a cheap validation check, so
`test_syscall_fuzz_k.c`'s `next_syscall_num()` deliberately rate-limits all
three to roughly 1-in-2000 draws rather than letting them land at their
natural ~1-in-29 frequency. **Any new `EXO_SYS_LAUNCH_*` number must be added
to that rate limit when it's introduced** — SCRUM-66 added
`EXO_SYS_LAUNCH_DOOM` to `exo_syscall.h` without updating the fuzz test, and
left unrated it turned roughly 1-in-29 of the million draws into a full
~200-page Doom image build, which is what blew the suite through the old
60s ceiling (and very nearly the new 120s one too). It runs last on purpose
(see that suite's own file comment), so it has no headroom to spare: any
suite added *before* it also eats into its share of the timeout, and adding
an unrated expensive syscall is worse still. Re-measure the full run rather
than assuming the headroom is still there.

**SCRUM-102 (ATA driver) is the concrete case that warning was written
for.** `docker-test`/`docker-ci` now attach a scratch IDE drive
(`-drive ...,if=ide`, `Makefile`) so the new `ata` suite can exercise real
hardware, and QEMU's own IDE probing at boot plus that suite's own runtime
were enough to push a clean run from comfortably under the old 120s ceiling
to ~112s — leaving almost no margin, and a genuinely truncated (not failed)
run on any CI host slower or more loaded than usual. Raised to 180s rather
than re-measuring a tighter number, since the next storage ticket
(`exo_disk_read`/`exo_disk_write`, SCRUM-103, plus the disk-image-attach
work in SCRUM-190) will only add more drive-touching suites here, not fewer.

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
Kernel Booted (x86_64)

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
| `KUNIT_MAX_SUITES` | 256 |
| `KUNIT_MAX_TESTS_PER_SUITE` | 128 |
| `KUNIT_NAME_LEN` | 64 bytes |

These can be increased in `src/kunit.h` if needed. The registry is a flat
static array, so the two counts multiply into `.bss` — raising the per-suite
cap is not free, and a suite approaching it is usually better split (see
`tests/kernel/test_libos_heap_k.c`, which registers two).

⚠️ **Exceeding them fails silently.** `CU_add_suite` returns `NULL` at the
ceiling, `CU_add_test(NULL, ...)` quietly does nothing, and the run still ends
in `ALL TESTS PASSED` — with an entire suite missing from the output. If a
suite you registered does not appear in the serial log, check the count here
before debugging anything else.

**Both ceilings bite, not just the suite one.** SCRUM-189's exofs suite
reached 66 tests against the then-64 per-suite cap and silently lost its last
two — including that ticket's own end-to-end acceptance test. Nothing in the
output said so; the only symptom was the summary reporting two fewer tests
than there were `CU_add_test` calls. **If `make docker-test`'s "Tests run"
count is lower than the number of `CU_add_test` calls you expect, check the
per-suite cap as well as the suite cap** — a suite that is present in the log
can still be missing tests from its tail.
