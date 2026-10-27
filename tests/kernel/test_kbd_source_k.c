/*
 * test_kbd_source_k.c -- per-context keyboard routing (SCRUM-199).
 *
 * src/kbd_source.c decides which ring a context's exo_kbd_poll pops. The
 * USB side runs on device-less slots (usb_kbd_test_attach()) fed through the
 * real report decoder, so no keyboard hardware is needed; the PS/2 side is
 * fed with kbd_enqueue(). Contexts are bare ids: the table keys on the id
 * alone, and the one syscall-boundary test sets the current context.
 */

#include "kunit.h"
#include "kbd_source.h"
#include "context.h"
#include "exo_syscall.h"
#include "syscall.h"
#include "usb_kbd.h"
#include "ps2.h"
#include "revoke.h"
#include "syscall_exit.h"
#include "vmm.h"

#include <stdint.h>

#define CTX_A ((page_owner_t)0x4100)
#define CTX_B ((page_owner_t)0x4101)

/* HID boot report with one key down (usage 0x04 == 'a'), and all keys up. */
static const uint8_t REPORT_A[8]  = { 0, 0, 0x04, 0, 0, 0, 0, 0 };
static const uint8_t REPORT_UP[8] = { 0 };

static int attach_usb(void)
{
    int idx = usb_kbd_test_attach();
    CU_ASSERT(idx >= 0);
    return idx;
}

static void setup(void)
{
    kbd_source_reset();
    kbd_reset();
}

static void test_claim_and_release(void)
{
    setup();
    CU_ASSERT_EQUAL(kbd_source_of(CTX_A), KBD_SRC_NONE);
    CU_ASSERT_EQUAL(kbd_source_claim(KBD_SRC_PS2, CTX_A), 0);
    CU_ASSERT_EQUAL(kbd_source_of(CTX_A), KBD_SRC_PS2);
    CU_ASSERT_EQUAL(kbd_source_owner(KBD_SRC_PS2), CTX_A);

    kbd_source_release_ctx(CTX_A);
    CU_ASSERT_EQUAL(kbd_source_of(CTX_A), KBD_SRC_NONE);
    CU_ASSERT_EQUAL(kbd_source_owner(KBD_SRC_PS2), PAGE_OWNER_FREE);
    kbd_source_reset();
}

static void test_double_claim_is_busy_and_first_survives(void)
{
    setup();
    CU_ASSERT_EQUAL(kbd_source_claim(KBD_SRC_PS2, CTX_A), 0);
    CU_ASSERT_EQUAL(kbd_source_claim(KBD_SRC_PS2, CTX_B), -EXO_EBUSY);
    CU_ASSERT_EQUAL(kbd_source_owner(KBD_SRC_PS2), CTX_A);
    CU_ASSERT_EQUAL(kbd_source_of(CTX_B), KBD_SRC_NONE);
    /* Re-claiming one's own source is not an error. */
    CU_ASSERT_EQUAL(kbd_source_claim(KBD_SRC_PS2, CTX_A), 0);
    kbd_source_reset();
}

static void test_reserved_context_ids_rejected(void)
{
    setup();
    CU_ASSERT_EQUAL(kbd_source_claim(KBD_SRC_PS2, PAGE_OWNER_FREE), -EXO_EINVAL);
    CU_ASSERT_EQUAL(kbd_source_claim(KBD_SRC_PS2, PAGE_OWNER_KERNEL), -EXO_EINVAL);
    CU_ASSERT_EQUAL(kbd_source_owner(KBD_SRC_PS2), PAGE_OWNER_FREE);
}

static void test_claim_discards_stale_events(void)
{
    setup();
    int u = attach_usb();
    kbd_event_t k = { .pressed = 1, .key = (uint8_t)KEY_Q, .modifiers = 0 };
    kbd_enqueue(k);
    usb_kbd_process_report(u, REPORT_A, 8);   /* key down, never released */

    CU_ASSERT_EQUAL(kbd_source_claim(KBD_SRC_PS2, CTX_A), 0);
    CU_ASSERT_EQUAL(kbd_source_claim(KBD_SRC_USB(u), CTX_B), 0);

    kbd_event_t ev;
    CU_ASSERT_EQUAL(kbd_source_poll(CTX_A, &ev), 0);
    CU_ASSERT_EQUAL(kbd_source_poll(CTX_B, &ev), 0);

    /* A repeat claim by the owner is not a new binding: it keeps its queue. */
    kbd_enqueue(k);
    CU_ASSERT_EQUAL(kbd_source_claim(KBD_SRC_PS2, CTX_A), 0);
    CU_ASSERT_EQUAL(kbd_source_poll(CTX_A, &ev), 1);

    usb_kbd_process_report(u, REPORT_UP, 8);
    kbd_source_reset();
    usb_kbd_test_detach(u);
}

static void test_unknown_source_rejected(void)
{
    setup();
    CU_ASSERT_EQUAL(kbd_source_claim(KBD_SRC_NONE, CTX_A), -EXO_ENOENT);
    CU_ASSERT_EQUAL(kbd_source_claim(KBD_SRC_MAX, CTX_A), -EXO_ENOENT);
    CU_ASSERT_EQUAL(kbd_source_claim(KBD_SRC_USB(usb_kbd_count()), CTX_A),
                    -EXO_ENOENT);
    CU_ASSERT_EQUAL(kbd_source_of(CTX_A), KBD_SRC_NONE);
}

static void test_claiming_another_source_gives_up_the_old_one(void)
{
    setup();
    int u = attach_usb();
    CU_ASSERT_EQUAL(kbd_source_claim(KBD_SRC_PS2, CTX_A), 0);
    CU_ASSERT_EQUAL(kbd_source_claim(KBD_SRC_USB(u), CTX_A), 0);
    CU_ASSERT_EQUAL(kbd_source_owner(KBD_SRC_PS2), PAGE_OWNER_FREE);
    CU_ASSERT_EQUAL(kbd_source_claim(KBD_SRC_PS2, CTX_B), 0);
    kbd_source_reset();
    usb_kbd_test_detach(u);
}

static void test_release_frees_source_for_rebind(void)
{
    setup();
    int u = attach_usb();
    CU_ASSERT_EQUAL(kbd_source_claim(KBD_SRC_USB(u), CTX_A), 0);
    CU_ASSERT_EQUAL(kbd_source_claim(KBD_SRC_USB(u), CTX_B), -EXO_EBUSY);
    kbd_source_release_ctx(CTX_A);
    CU_ASSERT_EQUAL(kbd_source_claim(KBD_SRC_USB(u), CTX_B), 0);
    kbd_source_reset();
    usb_kbd_test_detach(u);
}

/* The three ways a binding can end that do not go through the table API:
 * a destroyed context row (which is also what drops an unstarted hold),
 * revoke_all(), and context_teardown() (exo_exit and the fault path). Each
 * is followed by a rebind, so a leaked owner would show as -EXO_EBUSY. */
static page_owner_t make_context(void)
{
    uint64_t phys = 0;
    page_owner_t id = PAGE_OWNER_FREE;
    CU_ASSERT_EQUAL(vmm_create_address_space(&phys), VMM_OK);
    CU_ASSERT_EQUAL(context_create(phys, &id), CONTEXT_OK);
    return id;
}

static void test_context_destroy_releases_binding(void)
{
    setup();
    page_owner_t id = make_context();
    CU_ASSERT_EQUAL(kbd_source_claim(KBD_SRC_PS2, id), 0);
    CU_ASSERT_EQUAL(context_destroy(id), CONTEXT_OK);
    CU_ASSERT_EQUAL(kbd_source_owner(KBD_SRC_PS2), PAGE_OWNER_FREE);
    CU_ASSERT_EQUAL(kbd_source_claim(KBD_SRC_PS2, CTX_B), 0);
    kbd_source_reset();
}

static void test_revoke_releases_binding(void)
{
    setup();
    page_owner_t id = make_context();
    CU_ASSERT_EQUAL(kbd_source_claim(KBD_SRC_PS2, id), 0);
    (void)revoke_all(id);
    CU_ASSERT_EQUAL(kbd_source_owner(KBD_SRC_PS2), PAGE_OWNER_FREE);
    CU_ASSERT_EQUAL(kbd_source_claim(KBD_SRC_PS2, CTX_B), 0);
    kbd_source_reset();
    (void)context_destroy(id);
}

static void test_exit_teardown_releases_binding(void)
{
    setup();
    page_owner_t id = make_context();
    CU_ASSERT_EQUAL(kbd_source_claim(KBD_SRC_PS2, id), 0);
    CU_ASSERT_EQUAL(context_teardown(id), 0);
    CU_ASSERT_EQUAL(kbd_source_owner(KBD_SRC_PS2), PAGE_OWNER_FREE);
    CU_ASSERT_EQUAL(kbd_source_claim(KBD_SRC_PS2, CTX_B), 0);
    kbd_source_reset();
    (void)context_destroy(id);
}

static void test_events_reach_only_the_bound_context(void)
{
    setup();
    int u0 = attach_usb();
    int u1 = attach_usb();
    CU_ASSERT_EQUAL(kbd_source_claim(KBD_SRC_USB(u0), CTX_A), 0);
    CU_ASSERT_EQUAL(kbd_source_claim(KBD_SRC_USB(u1), CTX_B), 0);

    usb_kbd_process_report(u0, REPORT_A, 8);

    kbd_event_t ev;
    CU_ASSERT_EQUAL(kbd_source_poll(CTX_B, &ev), 0);
    CU_ASSERT_EQUAL(kbd_source_poll(CTX_A, &ev), 1);
    CU_ASSERT_EQUAL(ev.key, (uint8_t)KEY_A);
    CU_ASSERT_EQUAL(ev.pressed, 1);
    CU_ASSERT_EQUAL(kbd_source_poll(CTX_A, &ev), 0);

    usb_kbd_process_report(u1, REPORT_A, 8);
    CU_ASSERT_EQUAL(kbd_source_poll(CTX_A, &ev), 0);
    CU_ASSERT_EQUAL(kbd_source_poll(CTX_B, &ev), 1);

    usb_kbd_process_report(u0, REPORT_UP, 8);
    usb_kbd_process_report(u1, REPORT_UP, 8);
    while (kbd_source_poll(CTX_A, &ev)) {}
    while (kbd_source_poll(CTX_B, &ev)) {}

    kbd_source_reset();
    usb_kbd_test_detach(u1);
    usb_kbd_test_detach(u0);
}

static void test_ps2_and_usb_bound_separately(void)
{
    setup();
    int u = attach_usb();
    CU_ASSERT_EQUAL(kbd_source_claim(KBD_SRC_PS2, CTX_A), 0);
    CU_ASSERT_EQUAL(kbd_source_claim(KBD_SRC_USB(u), CTX_B), 0);

    kbd_event_t z = { .pressed = 1, .key = (uint8_t)KEY_Z, .modifiers = 0 };
    kbd_enqueue(z);
    usb_kbd_process_report(u, REPORT_A, 8);

    kbd_event_t ev;
    CU_ASSERT_EQUAL(kbd_source_poll(CTX_A, &ev), 1);
    CU_ASSERT_EQUAL(ev.key, (uint8_t)KEY_Z);
    CU_ASSERT_EQUAL(kbd_source_poll(CTX_A, &ev), 0);
    CU_ASSERT_EQUAL(kbd_source_poll(CTX_B, &ev), 1);
    CU_ASSERT_EQUAL(ev.key, (uint8_t)KEY_A);

    usb_kbd_process_report(u, REPORT_UP, 8);
    while (kbd_source_poll(CTX_B, &ev)) {}
    kbd_source_reset();
    usb_kbd_test_detach(u);
}

static void test_unbound_context_reads_ps2(void)
{
    setup();
    kbd_event_t k = { .pressed = 1, .key = (uint8_t)KEY_Q, .modifiers = 0 };
    kbd_enqueue(k);
    kbd_event_t ev;
    CU_ASSERT_EQUAL(kbd_source_poll(CTX_A, &ev), 1);
    CU_ASSERT_EQUAL(ev.key, (uint8_t)KEY_Q);
}

static void test_unbound_context_cannot_steal_owned_ps2(void)
{
    setup();
    CU_ASSERT_EQUAL(kbd_source_claim(KBD_SRC_PS2, CTX_A), 0);
    kbd_event_t k = { .pressed = 1, .key = (uint8_t)KEY_Q, .modifiers = 0 };
    kbd_enqueue(k);

    kbd_event_t ev;
    CU_ASSERT_EQUAL(kbd_source_poll(CTX_B, &ev), 0);
    CU_ASSERT_EQUAL(kbd_source_poll(CTX_A, &ev), 1);
    kbd_source_reset();
}

static void test_syscall_poll_routes_by_current_context(void)
{
    setup();
    /* The syscall boundary must use syscall_current_context(); the default
     * context (PAGE_OWNER_LIBOS) is unbound, so a ring owned by someone else
     * hides the PS/2 event from it. */
    page_owner_t saved = context_current();
    context_set_current(PAGE_OWNER_LIBOS);

    CU_ASSERT_EQUAL(kbd_source_claim(KBD_SRC_PS2, CTX_A), 0);
    kbd_event_t k = { .pressed = 1, .key = (uint8_t)KEY_Q, .modifiers = 0 };
    kbd_enqueue(k);

    kbd_event_t ev;
    CU_ASSERT_EQUAL(kbd_source_poll(syscall_current_context(), &ev), 0);
    kbd_source_release_ctx(CTX_A);
    CU_ASSERT_EQUAL(kbd_source_poll(syscall_current_context(), &ev), 1);

    context_set_current(saved);
    kbd_source_reset();
}

void suite_kbd_source_tests(CU_pSuite s)
{
    CU_add_test(s, "claim and release", test_claim_and_release);
    CU_add_test(s, "double claim is EBUSY, first owner unaffected",
                test_double_claim_is_busy_and_first_survives);
    CU_add_test(s, "reserved context ids rejected",
                test_reserved_context_ids_rejected);
    CU_add_test(s, "claim discards stale events",
                test_claim_discards_stale_events);
    CU_add_test(s, "unknown source rejected", test_unknown_source_rejected);
    CU_add_test(s, "claiming another source gives up the old one",
                test_claiming_another_source_gives_up_the_old_one);
    CU_add_test(s, "release frees the source for rebind",
                test_release_frees_source_for_rebind);
    CU_add_test(s, "context_destroy releases the binding",
                test_context_destroy_releases_binding);
    CU_add_test(s, "revoke_all releases the binding",
                test_revoke_releases_binding);
    CU_add_test(s, "exit teardown releases the binding",
                test_exit_teardown_releases_binding);
    CU_add_test(s, "events reach only the bound context",
                test_events_reach_only_the_bound_context);
    CU_add_test(s, "PS/2 and USB bound to different contexts",
                test_ps2_and_usb_bound_separately);
    CU_add_test(s, "unbound context reads PS/2",
                test_unbound_context_reads_ps2);
    CU_add_test(s, "unbound context cannot steal an owned PS/2 ring",
                test_unbound_context_cannot_steal_owned_ps2);
    CU_add_test(s, "poll uses the current context",
                test_syscall_poll_routes_by_current_context);
}
