/*
 * test_exo_syscall_k.c — LibOS-side view of the syscall ABI (SCRUM-24).
 *
 * Acceptance criteria under test:
 *   - the header compiles, and compiles as the LibOS sees it (no EXO_KERNEL),
 *     which is the view that also instantiates the inline `syscall` stubs
 *   - the numbers and struct layouts match docs/syscall_spec.md §3.2
 *
 * Nothing here issues a syscall: there is no dispatcher yet (SCRUM-32), so a
 * real `syscall` would fault.  The stubs are referenced by address instead,
 * which still forces the compiler to emit and the linker to resolve them.
 *
 * The matching kernel-side view (EXO_KERNEL defined) lives in
 * test_exo_syscall_kview_k.c.
 */

/* Kernel TUs, tests included, are compiled with -DEXO_KERNEL (see
 * docker/scripts/build.sh).  This is the one file that wants the LibOS view,
 * so it drops the define before the first include of the header -- which is
 * only sound because nothing above pulls exo_syscall.h in transitively
 * (kunit.h includes string.h and nothing else); #pragma once would make a
 * later #undef a no-op. */
#undef EXO_KERNEL

#include "kunit.h"
#include "exo_syscall.h"

/* ---- Syscall numbers --------------------------------------------------- */

static void test_numbers_match_spec(void)
{
    CU_ASSERT_EQUAL(EXO_SYS_PAGE_ALLOC,    0);
    CU_ASSERT_EQUAL(EXO_SYS_PAGE_FREE,     1);
    CU_ASSERT_EQUAL(EXO_SYS_PAGE_MAP,      2);
    CU_ASSERT_EQUAL(EXO_SYS_PAGE_UNMAP,    3);
    CU_ASSERT_EQUAL(EXO_SYS_FB_ACQUIRE,    4);
    CU_ASSERT_EQUAL(EXO_SYS_GET_TICKS,     5);
    CU_ASSERT_EQUAL(EXO_SYS_KBD_POLL,      6);
    CU_ASSERT_EQUAL(EXO_SYS_MOUSE_POLL,    7);
    CU_ASSERT_EQUAL(EXO_SYS_SERIAL_WRITE,  8);
    CU_ASSERT_EQUAL(EXO_SYS_FILE_OPEN,     9);
    CU_ASSERT_EQUAL(EXO_SYS_FILE_CLOSE,   10);
    CU_ASSERT_EQUAL(EXO_SYS_FILE_READ,    11);
    CU_ASSERT_EQUAL(EXO_SYS_FILE_WRITE,   12);
    CU_ASSERT_EQUAL(EXO_SYS_FILE_SEEK,    13);
    CU_ASSERT_EQUAL(EXO_SYS_FILE_STAT,    14);
    CU_ASSERT_EQUAL(EXO_SYS_FILE_REMOVE,  15);
    CU_ASSERT_EQUAL(EXO_SYS_FILE_RENAME,  16);
    CU_ASSERT_EQUAL(EXO_SYS_SOUND_TONE,   17);
    CU_ASSERT_EQUAL(EXO_SYS_SOUND_STOP,   18);
    CU_ASSERT_EQUAL(EXO_SYS_YIELD,        19);
    CU_ASSERT_EQUAL(EXO_SYS_EXIT,         20);
}

/* The dispatcher will range-check against EXO_SYS_COUNT, so it has to stay one
 * past the last number — and the spec's total is 21 syscalls. */
static void test_count_is_one_past_last(void)
{
    CU_ASSERT_EQUAL(EXO_SYS_COUNT, EXO_SYS_EXIT + 1);
    CU_ASSERT_EQUAL(EXO_SYS_COUNT, 21);
}

/* Two syscalls sharing a number would route silently to the wrong handler. */
static void test_numbers_are_unique(void)
{
    static const int numbers[] = {
        EXO_SYS_PAGE_ALLOC,   EXO_SYS_PAGE_FREE,   EXO_SYS_PAGE_MAP,
        EXO_SYS_PAGE_UNMAP,   EXO_SYS_FB_ACQUIRE,  EXO_SYS_GET_TICKS,
        EXO_SYS_KBD_POLL,     EXO_SYS_MOUSE_POLL,  EXO_SYS_SERIAL_WRITE,
        EXO_SYS_FILE_OPEN,    EXO_SYS_FILE_CLOSE,  EXO_SYS_FILE_READ,
        EXO_SYS_FILE_WRITE,   EXO_SYS_FILE_SEEK,   EXO_SYS_FILE_STAT,
        EXO_SYS_FILE_REMOVE,  EXO_SYS_FILE_RENAME, EXO_SYS_SOUND_TONE,
        EXO_SYS_SOUND_STOP,   EXO_SYS_YIELD,       EXO_SYS_EXIT,
    };
    /* 64-bit so the mask keeps working as the table grows; the assert makes
     * the ceiling explicit rather than letting the shift go undefined. */
    uint64_t seen = 0;   /* bit i set once number i has been visited */
    unsigned i;

    _Static_assert(EXO_SYS_COUNT < 64, "uniqueness mask is 64 bits wide");

    CU_ASSERT_EQUAL(sizeof(numbers) / sizeof(numbers[0]), EXO_SYS_COUNT);

    for (i = 0; i < sizeof(numbers) / sizeof(numbers[0]); i++) {
        /* CU_ASSERT_* records a failure and keeps going, so an out-of-range
         * number would otherwise reach the shift below and invoke UB in the
         * very test written to catch it. */
        CU_ASSERT_TRUE(numbers[i] >= 0 && numbers[i] < (int)EXO_SYS_COUNT);
        if (numbers[i] < 0 || numbers[i] >= (int)EXO_SYS_COUNT) {
            continue;
        }
        CU_ASSERT_FALSE(seen & (UINT64_C(1) << numbers[i]));
        seen |= UINT64_C(1) << numbers[i];
    }

    /* Dense: every number below EXO_SYS_COUNT is claimed exactly once. */
    CU_ASSERT_EQUAL(seen, (UINT64_C(1) << EXO_SYS_COUNT) - 1u);
}

/* ---- Shared struct layout ---------------------------------------------- */

static void test_fb_info_layout(void)
{
    CU_ASSERT_EQUAL(sizeof(exo_fb_info_t), 24);
    CU_ASSERT_EQUAL(__builtin_offsetof(exo_fb_info_t, phys_addr),  0);
    CU_ASSERT_EQUAL(__builtin_offsetof(exo_fb_info_t, width),      8);
    CU_ASSERT_EQUAL(__builtin_offsetof(exo_fb_info_t, height),    12);
    CU_ASSERT_EQUAL(__builtin_offsetof(exo_fb_info_t, pitch),     16);
    CU_ASSERT_EQUAL(__builtin_offsetof(exo_fb_info_t, bpp),       20);
}

static void test_kbd_event_layout(void)
{
    CU_ASSERT_EQUAL(sizeof(exo_kbd_event_t), 4);
    CU_ASSERT_EQUAL(__builtin_offsetof(exo_kbd_event_t, pressed),   0);
    CU_ASSERT_EQUAL(__builtin_offsetof(exo_kbd_event_t, key),       1);
    CU_ASSERT_EQUAL(__builtin_offsetof(exo_kbd_event_t, modifiers), 2);
    CU_ASSERT_EQUAL(__builtin_offsetof(exo_kbd_event_t, reserved),  3);
}

/* Doom binds shift, ctrl and alt without caring which side of the keyboard the
 * key is on, so the pair masks have to cover both bits and nothing else. */
static void test_kbd_modifier_bits(void)
{
    CU_ASSERT_EQUAL(EXO_MOD_LSHIFT, 0x01);
    CU_ASSERT_EQUAL(EXO_MOD_RSHIFT, 0x02);
    CU_ASSERT_EQUAL(EXO_MOD_LCTRL,  0x04);
    CU_ASSERT_EQUAL(EXO_MOD_RCTRL,  0x08);
    CU_ASSERT_EQUAL(EXO_MOD_LALT,   0x10);
    CU_ASSERT_EQUAL(EXO_MOD_RALT,   0x20);

    CU_ASSERT_EQUAL(EXO_MOD_SHIFT, 0x03);
    CU_ASSERT_EQUAL(EXO_MOD_CTRL,  0x0C);
    CU_ASSERT_EQUAL(EXO_MOD_ALT,   0x30);

    /* No bit belongs to two modifiers. */
    CU_ASSERT_EQUAL(EXO_MOD_SHIFT & EXO_MOD_CTRL, 0);
    CU_ASSERT_EQUAL(EXO_MOD_SHIFT & EXO_MOD_ALT,  0);
    CU_ASSERT_EQUAL(EXO_MOD_CTRL  & EXO_MOD_ALT,  0);

    /* The whole mask fits the byte the ABI struct gives it. */
    CU_ASSERT_EQUAL((EXO_MOD_SHIFT | EXO_MOD_CTRL | EXO_MOD_ALT) & ~0xFFu, 0);
}

static void test_mouse_state_layout(void)
{
    CU_ASSERT_EQUAL(sizeof(exo_mouse_state_t), 6);
    CU_ASSERT_EQUAL(__builtin_offsetof(exo_mouse_state_t, dx),      0);
    CU_ASSERT_EQUAL(__builtin_offsetof(exo_mouse_state_t, dy),      2);
    CU_ASSERT_EQUAL(__builtin_offsetof(exo_mouse_state_t, buttons), 4);

    /* Deltas are signed — a leftward mouse move must not read as +65280. */
    {
        exo_mouse_state_t st;
        st.dx = -256;
        CU_ASSERT_TRUE(st.dx < 0);
    }
}

/* ---- Argument constants ------------------------------------------------- */

static void test_argument_constants(void)
{
    CU_ASSERT_EQUAL(EXO_O_RDONLY, 0);
    CU_ASSERT_EQUAL(EXO_O_WRONLY, 1);
    CU_ASSERT_EQUAL(EXO_O_RDWR,   2);

    /* Same ordering as stdio's SEEK_*, so the FILE* shim can pass them
     * straight through. */
    CU_ASSERT_EQUAL(EXO_SEEK_SET, 0);
    CU_ASSERT_EQUAL(EXO_SEEK_CUR, 1);
    CU_ASSERT_EQUAL(EXO_SEEK_END, 2);

    /* Page flags are a mask, not an enum. */
    CU_ASSERT_EQUAL(EXO_PAGE_READ | EXO_PAGE_WRITE | EXO_PAGE_USER
                    | EXO_PAGE_EXEC, 0xF);

    /* Error codes are positive here and returned negated. */
    CU_ASSERT_TRUE(EXO_ENOMEM > 0);
    CU_ASSERT_TRUE(EXO_EINVAL > 0);
    CU_ASSERT_TRUE(EXO_ENOSYS > 0);
}

/* ---- Stub instantiation ------------------------------------------------- */
/*
 * Referencing every wrapper by address forces the compiler to emit the inline
 * asm and the linker to resolve it, so a malformed constraint or a wrapper
 * calling the wrong exo_syscallN arity fails the build rather than lurking
 * until SCRUM-32.  volatile keeps the array from being optimised away.
 *
 * That only covers exo_syscall0..3: no syscall in §3.2 takes more than three
 * arguments, so the 4-, 5- and 6-argument stubs have no caller and GCC never
 * checks the asm in an uninstantiated static inline — a bogus constraint in
 * them compiles silently.  These thunks give them a caller so the R10/R8/R9
 * register variables are validated too.  They are never invoked.
 */
static int64_t exo_syscall4_thunk(uint64_t n, uint64_t a, uint64_t b,
                                  uint64_t c, uint64_t d)
{
    return exo_syscall4(n, a, b, c, d);
}

static int64_t exo_syscall5_thunk(uint64_t n, uint64_t a, uint64_t b,
                                  uint64_t c, uint64_t d, uint64_t e)
{
    return exo_syscall5(n, a, b, c, d, e);
}

static int64_t exo_syscall6_thunk(uint64_t n, uint64_t a, uint64_t b,
                                  uint64_t c, uint64_t d, uint64_t e,
                                  uint64_t f)
{
    return exo_syscall6(n, a, b, c, d, e, f);
}

static void *const volatile raw_stub_addresses[] = {
    (void *)exo_syscall4_thunk,
    (void *)exo_syscall5_thunk,
    (void *)exo_syscall6_thunk,
};
static void *const volatile stub_addresses[] = {
    (void *)exo_page_alloc,   (void *)exo_page_free,   (void *)exo_page_map,
    (void *)exo_page_unmap,   (void *)exo_fb_acquire,  (void *)exo_get_ticks,
    (void *)exo_kbd_poll,     (void *)exo_mouse_poll,  (void *)exo_serial_write,
    (void *)exo_file_open,    (void *)exo_file_close,  (void *)exo_file_read,
    (void *)exo_file_write,   (void *)exo_file_seek,   (void *)exo_file_stat,
    (void *)exo_file_remove,  (void *)exo_file_rename, (void *)exo_sound_tone,
    (void *)exo_sound_stop,   (void *)exo_yield,       (void *)exo_exit,
};

static void test_every_syscall_has_a_stub(void)
{
    unsigned i;

    CU_ASSERT_EQUAL(sizeof(stub_addresses) / sizeof(stub_addresses[0]),
                    EXO_SYS_COUNT);

    for (i = 0; i < sizeof(stub_addresses) / sizeof(stub_addresses[0]); i++) {
        CU_ASSERT_PTR_NOT_NULL(stub_addresses[i]);
    }

    /* The 4-6 argument raw stubs have no wrapper of their own; these keep them
     * compiled and linked so their R10/R8/R9 constraints stay checked. */
    for (i = 0; i < sizeof(raw_stub_addresses) / sizeof(raw_stub_addresses[0]);
         i++) {
        CU_ASSERT_PTR_NOT_NULL(raw_stub_addresses[i]);
    }
}

void suite_exo_syscall_tests(CU_pSuite s)
{
    CU_add_test(s, "numbers_match_spec",   test_numbers_match_spec);
    CU_add_test(s, "count_one_past_last",  test_count_is_one_past_last);
    CU_add_test(s, "numbers_unique_dense", test_numbers_are_unique);
    CU_add_test(s, "fb_info_layout",       test_fb_info_layout);
    CU_add_test(s, "kbd_event_layout",     test_kbd_event_layout);
    CU_add_test(s, "kbd_modifier_bits",    test_kbd_modifier_bits);
    CU_add_test(s, "mouse_state_layout",   test_mouse_state_layout);
    CU_add_test(s, "argument_constants",   test_argument_constants);
    CU_add_test(s, "stubs_link",           test_every_syscall_has_a_stub);
}
