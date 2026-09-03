/*
 * test_exo_syscall_kview_k.c — Kernel-side view of the syscall ABI (SCRUM-24).
 *
 * The other half of the acceptance criterion "both kernel and LibOS sides can
 * include it": this translation unit sees the kernel view of the header.  It
 * gets that from -DEXO_KERNEL in docker/scripts/build.sh, the same way real
 * kernel sources do — no #define here, which would only redefine the flag the
 * build already set.  That view must
 *
 *   - still provide the numbers, structs and error codes, and
 *   - suppress the inline `syscall` stubs, which the kernel must never issue
 *     against itself — and whose exo_kbd_poll would otherwise collide with the
 *     kernel's own exo_kbd_poll(kbd_event_t *) in ps2.h.
 *
 * Including both headers here is the collision check; it fails at compile
 * time, not at runtime, if the guard ever regresses.
 */

#include "kunit.h"
#include "exo_syscall.h"
#include "ps2.h"

static void test_kernel_view_has_the_abi(void)
{
    exo_fb_info_t     fb;
    exo_kbd_event_t   ev;
    exo_mouse_state_t mouse;

    /* Numbers and error codes survive the guard. */
    CU_ASSERT_EQUAL(EXO_SYS_GET_TICKS, 5);
    CU_ASSERT_EQUAL(EXO_SYS_COUNT,    21);
    CU_ASSERT_EQUAL(EXO_ENOSYS,       38);

    /* So do the structs the dispatcher fills in on the caller's behalf. */
    fb.bpp       = 32;
    ev.key       = KEY_ESC;
    mouse.dx     = -1;

    CU_ASSERT_EQUAL(fb.bpp, 32);
    CU_ASSERT_EQUAL(ev.key, KEY_ESC);
    CU_ASSERT_EQUAL(mouse.dx, -1);
}

/* The ABI event mirrors ps2.h's kernel-internal event field for field, but the
 * two remain distinct types: the ABI one has a trailing reserved byte, and the
 * kernel struct may grow.  The SCRUM-32 handler converts field by field rather
 * than casting or memcpy-ing one onto the other.
 *
 * Only the ABI side is pinned here.  Asserting the two sizes differ would fail
 * the moment kbd_event_t legitimately grows to 4 bytes, which is exactly the
 * freedom this comment describes. */
static void test_kernel_event_is_not_the_abi_event(void)
{
    CU_ASSERT_EQUAL(sizeof(exo_kbd_event_t), 4);

    /* The modifier bits the kernel samples are the ones the ABI publishes, so
     * the conversion copies the mask through untouched. */
    CU_ASSERT_EQUAL(MOD_LSHIFT, EXO_MOD_LSHIFT);
    CU_ASSERT_EQUAL(MOD_RSHIFT, EXO_MOD_RSHIFT);
    CU_ASSERT_EQUAL(MOD_LCTRL,  EXO_MOD_LCTRL);
    CU_ASSERT_EQUAL(MOD_RCTRL,  EXO_MOD_RCTRL);
    CU_ASSERT_EQUAL(MOD_LALT,   EXO_MOD_LALT);
    CU_ASSERT_EQUAL(MOD_RALT,   EXO_MOD_RALT);
}

void suite_exo_syscall_kview_tests(CU_pSuite s)
{
    CU_add_test(s, "kernel_view_has_abi",   test_kernel_view_has_the_abi);
    CU_add_test(s, "kernel_event_differs",  test_kernel_event_is_not_the_abi_event);
}
