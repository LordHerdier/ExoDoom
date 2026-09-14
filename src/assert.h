#pragma once

/*
 * Freestanding <assert.h> (SCRUM-64).
 *
 * Only src/doom/sha1.c includes this, and only for the standard `assert`
 * macro.  There is no `abort()` in the shim yet (see src/stdlib.h), and a
 * kernel has nowhere to abort *to*, so a failing assertion reports on serial
 * and halts the CPU rather than unwinding.
 *
 * NDEBUG is honoured the standard way: define it and `assert` compiles to a
 * no-op expression that still type-checks its argument.
 */

#ifdef NDEBUG

#define assert(expr) ((void)0)

#else

/* Declared rather than included: pulling src/stdio.h in here would put printf
 * on the include path of every TU that asserts, and <assert.h> is specified to
 * define nothing but the macro. */
int printf(const char *fmt, ...);

/*
 * __FILE__/__LINE__/__func__ are all compiler-provided, so this needs nothing
 * from the shim beyond printf.  The `for(;;)` after the halt is what tells the
 * compiler this path does not return -- without it, callers that assert on an
 * invariant and then rely on it get "may be used uninitialized" warnings, and
 * -Wall -Wextra is on for src/doom/.
 */
#define assert(expr)                                                          \
    ((expr) ? (void)0                                                         \
            : (assert_fail(#expr, __FILE__, __LINE__, __func__)))

static inline void assert_fail(const char *expr, const char *file, int line,
                               const char *func)
{
    printf("assertion failed: %s\n  at %s:%d in %s\n", expr, file, line, func);
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

#endif /* NDEBUG */
