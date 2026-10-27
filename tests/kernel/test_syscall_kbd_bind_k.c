/*
 * test_syscall_kbd_bind_k.c — exo_kbd_bind (#44, SCRUM-297): a launcher
 * chooses which keyboard the context it holds reads.
 *
 * Every case drives the real handler through exo_syscall_dispatch(), on
 * contexts built by real held launches (#36 with EXO_LAUNCH_HOLD) of the
 * launch-probe package, the way test_syscall_pane_k.c does. USB keyboards are
 * device-less slots (usb_kbd_test_attach()) fed through the report decoder;
 * the PS/2 side is fed with kbd_enqueue(). What each context then reads is
 * kbd_source_poll(), the routing exo_kbd_poll applies to its caller.
 *
 * A launcher holds one context at a time (holding another drops the first),
 * so a pair of bound contexts is built by binding one, starting it, and
 * holding the next.
 */

#include "kunit.h"
#include "context.h"
#include "vmm.h"
#include "page_alloc.h"
#include "libos_launch.h"
#include "exo_pkg.h"
#include "exo_syscall.h"
#include "syscall.h"
#include "syscall_exit.h"
#include "string.h"
#include "fb_shadow.h"
#include "kbd_source.h"
#include "usb_kbd.h"
#include "ps2.h"
#include "revoke.h"

#include <stdint.h>

extern void libos_launch_probe(void);
extern void libos_launch_probe_end(void);

/* Apart from the other launch suites' scratch ranges (test_launch_pkg_k.c
 * +0x2F000000, test_launch_multi_k.c +0x2F100000, test_syscall_pane_k.c
 * +0x2F200000). */
#define BIND_PKG_VADDR (EXO_USER_VA_BASE + 0x2F300000ULL)

#define BIND_CLEANUP_SCAN_IDS 4096

static const uint8_t REPORT_A[8]  = { 0, 0, 0x04, 0, 0, 0, 0, 0 };
static const uint8_t REPORT_UP[8] = { 0 };

int syscall_kbd_bind_suite_init(void)
{
    kbd_source_reset();
    return 0;
}

/* A failed assertion mid-test must not leak a context or a binding into a
 * later suite. */
int syscall_kbd_bind_suite_cleanup(void)
{
    for (uint32_t offset = 0; offset < BIND_CLEANUP_SCAN_IDS; offset++) {
        page_owner_t id = (page_owner_t)(PAGE_OWNER_LIBOS + offset);
        if (id != PAGE_OWNER_LIBOS && context_lookup(id) != NULL) {
            fb_shadow_release(id);
            page_reclaim_all(id);
            context_destroy(id);
        }
    }
    context_switch_pending = 0;
    context_set_current(PAGE_OWNER_LIBOS);
    kbd_source_reset();
    kbd_reset();
    return 0;
}

static int64_t do_bind(uint64_t ctx, uint64_t source)
{
    return exo_syscall_dispatch(EXO_SYS_KBD_BIND, ctx, source, 0, 0, 0, 0);
}

static size_t probe_len(void)
{
    return (uintptr_t)&libos_launch_probe_end - (uintptr_t)&libos_launch_probe;
}

/* The scratch page the package is written to, owned by PAGE_OWNER_LIBOS (the
 * context the suite runs as) and mapped through the real #1/#2. */
static uint64_t map_package(size_t *total)
{
    int64_t paddr = exo_syscall_dispatch(EXO_SYS_PAGE_ALLOC, 0, 0, 0, 0, 0, 0);
    CU_ASSERT_TRUE(paddr > 0);
    if (paddr <= 0)
        return 0;
    CU_ASSERT_EQUAL(exo_syscall_dispatch(EXO_SYS_PAGE_MAP, BIND_PKG_VADDR,
                                         (uint64_t)paddr,
                                         EXO_PAGE_READ | EXO_PAGE_WRITE |
                                             EXO_PAGE_USER, 0, 0, 0), 0);
    memset((void *)(uintptr_t)BIND_PKG_VADDR, 0, VMM_PAGE_SIZE);

    exo_pkg_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic             = EXO_PKG_MAGIC;
    h.version           = EXO_PKG_VERSION;
    h.code_len          = (uint32_t)probe_len();
    h.stack_pages       = EXO_PKG_MAX_STACK_PAGES;
    h.min_pages         = 20;
    h.recommended_pages = 36;
    CU_ASSERT_EQUAL(exo_pkg_pack((void *)(uintptr_t)BIND_PKG_VADDR,
                                 VMM_PAGE_SIZE, &h,
                                 (const void *)&libos_launch_probe, NULL,
                                 total), 0);
    return (uint64_t)paddr;
}

static void unmap_package(uint64_t paddr)
{
    CU_ASSERT_EQUAL(exo_syscall_dispatch(EXO_SYS_PAGE_UNMAP, BIND_PKG_VADDR,
                                         0, 0, 0, 0, 0), 0);
    CU_ASSERT_EQUAL(exo_syscall_dispatch(EXO_SYS_PAGE_FREE, paddr,
                                         0, 0, 0, 0, 0), 0);
}

/* A real, live, current "launcher", as test_launch_pkg_k.c's
 * make_foreground() builds one. */
static page_owner_t make_launcher(void)
{
    page_owner_t id = PAGE_OWNER_FREE;
    CU_ASSERT_EQUAL(context_create(vmm_kernel_pml4(), &id), CONTEXT_OK);
    libos_image_t img;
    CU_ASSERT_EQUAL(libos_build_image(id, (const void *)&libos_launch_probe,
                                      probe_len(), 0, 0, 0, &img), VMM_OK);
    CU_ASSERT_EQUAL(context_prime(id, img.entry_vaddr, img.stack_top_vaddr),
                    CONTEXT_OK);
    context_set_current(id);
    context_set_state(id, CONTEXT_STATE_RUNNING);
    return id;
}

static void drop_launcher(page_owner_t l)
{
    context_switch_pending = 0;
    context_set_current(PAGE_OWNER_LIBOS);
    revoke_all(l);
    context_destroy(l);
}

/* Hold a launch of the probe package as the current context; its id. */
static page_owner_t hold(size_t total)
{
    int64_t id = exo_syscall_dispatch(EXO_SYS_LAUNCH_PKG, BIND_PKG_VADDR, total,
                                      EXO_LAUNCH_HOLD, 0, 0, 0);
    CU_ASSERT_TRUE(id > 0);
    return id > 0 ? (page_owner_t)id : PAGE_OWNER_FREE;
}


/* Bind a hold, start it (the binding outlives the hold) and come back as the
 * launcher, as test_syscall_pane_k.c does. */
static page_owner_t hold_bind_start(size_t total, page_owner_t launcher,
                                    uint64_t source)
{
    page_owner_t c = hold(total);
    CU_ASSERT_EQUAL(do_bind(c, source), 0);
    CU_ASSERT_EQUAL(exo_syscall_dispatch(EXO_SYS_LAUNCH_START, c, 0, 0, 0, 0, 0),
                    0);
    context_switch_pending = 0;
    context_set_current(launcher);
    return c;
}

static int attach_usb(void)
{
    int idx = usb_kbd_test_attach();
    CU_ASSERT(idx >= 0);
    return idx;
}

static void test_handler_is_bound(void)
{
    CU_ASSERT_PTR_NOT_NULL(exo_syscall_handler(EXO_SYS_KBD_BIND));
}

/* The launcher binds each context to its own keyboard; keys on each reach
 * only that context. */
static void test_two_contexts_read_only_their_own_keyboard(void)
{
    kbd_source_reset();
    kbd_reset();
    size_t total = 0;
    uint64_t paddr = map_package(&total);
    int u = attach_usb();
    page_owner_t l = make_launcher();
    page_owner_t a = hold_bind_start(total, l, EXO_KBD_SRC_PS2);
    page_owner_t b = hold(total);

    CU_ASSERT_EQUAL(do_bind(b, EXO_KBD_SRC_USB(u)), 0);
    CU_ASSERT_EQUAL(kbd_source_owner(KBD_SRC_PS2), a);
    CU_ASSERT_EQUAL(kbd_source_owner(KBD_SRC_USB(u)), b);

    kbd_event_t z = { .pressed = 1, .key = (uint8_t)KEY_Z, .modifiers = 0 };
    kbd_enqueue(z);
    usb_kbd_process_report(u, REPORT_A, 8);

    kbd_event_t ev;
    CU_ASSERT_EQUAL(kbd_source_poll(a, &ev), 1);
    CU_ASSERT_EQUAL(ev.key, (uint8_t)KEY_Z);
    CU_ASSERT_EQUAL(kbd_source_poll(a, &ev), 0);
    CU_ASSERT_EQUAL(kbd_source_poll(b, &ev), 1);
    CU_ASSERT_EQUAL(ev.key, (uint8_t)KEY_A);
    CU_ASSERT_EQUAL(kbd_source_poll(b, &ev), 0);

    usb_kbd_process_report(u, REPORT_UP, 8);
    while (kbd_source_poll(b, &ev)) {}

    revoke_all(a);
    context_destroy(a);
    revoke_all(b);
    context_destroy(b);
    drop_launcher(l);
    usb_kbd_test_detach(u);
    unmap_package(paddr);
}

static void test_binding_twice_is_busy_and_first_survives(void)
{
    kbd_source_reset();
    size_t total = 0;
    uint64_t paddr = map_package(&total);
    page_owner_t l = make_launcher();
    page_owner_t a = hold_bind_start(total, l, EXO_KBD_SRC_PS2);
    page_owner_t b = hold(total);

    CU_ASSERT_EQUAL(do_bind(b, EXO_KBD_SRC_PS2), -EXO_EBUSY);
    CU_ASSERT_EQUAL(kbd_source_owner(KBD_SRC_PS2), a);
    CU_ASSERT_EQUAL(kbd_source_of(b), KBD_SRC_NONE);

    revoke_all(a);
    context_destroy(a);
    revoke_all(b);
    context_destroy(b);
    drop_launcher(l);
    unmap_package(paddr);
}

static void test_unbind_frees_the_source(void)
{
    kbd_source_reset();
    size_t total = 0;
    uint64_t paddr = map_package(&total);
    page_owner_t l = make_launcher();
    page_owner_t a = hold(total);

    CU_ASSERT_EQUAL(do_bind(a, EXO_KBD_SRC_PS2), 0);
    CU_ASSERT_EQUAL(do_bind(a, EXO_KBD_SRC_NONE), 0);
    CU_ASSERT_EQUAL(kbd_source_owner(KBD_SRC_PS2), PAGE_OWNER_FREE);
    /* Unbinding what is not bound is still success. */
    CU_ASSERT_EQUAL(do_bind(a, EXO_KBD_SRC_NONE), 0);

    /* Holding another context drops `a`; the source is free for it. */
    page_owner_t b = hold(total);
    CU_ASSERT_EQUAL(do_bind(b, EXO_KBD_SRC_PS2), 0);
    CU_ASSERT_EQUAL(kbd_source_owner(KBD_SRC_PS2), b);

    revoke_all(b);
    context_destroy(b);
    drop_launcher(l);
    unmap_package(paddr);
}

/* Each way a binding can end frees the source: a dropped hold, revocation,
 * and a normal exit. A leaked owner would show as -EXO_EBUSY on the rebind. */
static void test_dropped_revoked_and_exited_contexts_free_the_source(void)
{
    kbd_source_reset();
    size_t total = 0;
    uint64_t paddr = map_package(&total);
    page_owner_t l = make_launcher();

    page_owner_t a = hold(total);
    CU_ASSERT_EQUAL(do_bind(a, EXO_KBD_SRC_PS2), 0);
    context_destroy(a);                       /* a dropped, unstarted hold */
    CU_ASSERT_EQUAL(kbd_source_owner(KBD_SRC_PS2), PAGE_OWNER_FREE);

    page_owner_t b = hold(total);
    CU_ASSERT_EQUAL(do_bind(b, EXO_KBD_SRC_PS2), 0);
    revoke_all(b);
    CU_ASSERT_EQUAL(kbd_source_owner(KBD_SRC_PS2), PAGE_OWNER_FREE);
    context_destroy(b);

    page_owner_t c = hold(total);
    CU_ASSERT_EQUAL(do_bind(c, EXO_KBD_SRC_PS2), 0);
    CU_ASSERT_EQUAL(context_teardown(c), 0);  /* exo_exit's reclaim */
    CU_ASSERT_EQUAL(kbd_source_owner(KBD_SRC_PS2), PAGE_OWNER_FREE);
    context_destroy(c);

    page_owner_t d = hold(total);
    CU_ASSERT_EQUAL(do_bind(d, EXO_KBD_SRC_PS2), 0);
    revoke_all(d);
    context_destroy(d);

    drop_launcher(l);
    unmap_package(paddr);
}

static void test_only_the_launcher_may_bind_its_hold(void)
{
    kbd_source_reset();
    size_t total = 0;
    uint64_t paddr = map_package(&total);
    page_owner_t l = make_launcher();
    page_owner_t c = hold(total);
    CU_ASSERT_EQUAL(do_bind(c, EXO_KBD_SRC_PS2), 0);

    page_owner_t other = make_launcher();      /* now the current context */
    CU_ASSERT_EQUAL(do_bind(c, EXO_KBD_SRC_NONE), -EXO_EPERM);
    CU_ASSERT_EQUAL(do_bind(c, EXO_KBD_SRC_PS2), -EXO_EPERM);
    CU_ASSERT_EQUAL(kbd_source_owner(KBD_SRC_PS2), c);

    /* The program itself is not its own launcher either. */
    context_set_current(c);
    CU_ASSERT_EQUAL(do_bind(c, EXO_KBD_SRC_NONE), -EXO_EPERM);
    CU_ASSERT_EQUAL(kbd_source_owner(KBD_SRC_PS2), c);

    context_set_current(other);
    revoke_all(other);
    context_destroy(other);
    context_set_current(l);
    revoke_all(c);
    context_destroy(c);
    drop_launcher(l);
    unmap_package(paddr);
}

static void test_not_a_hold_is_enoent(void)
{
    kbd_source_reset();
    page_owner_t l = make_launcher();
    CU_ASSERT_EQUAL(do_bind(l, EXO_KBD_SRC_PS2), -EXO_ENOENT);  /* not a hold */
    CU_ASSERT_EQUAL(do_bind(0, EXO_KBD_SRC_PS2), -EXO_ENOENT);
    CU_ASSERT_EQUAL(do_bind(0xFFFFull, EXO_KBD_SRC_PS2), -EXO_ENOENT);
    CU_ASSERT_EQUAL(do_bind(1ull << 32, EXO_KBD_SRC_PS2), -EXO_ENOENT);
    CU_ASSERT_EQUAL(kbd_source_owner(KBD_SRC_PS2), PAGE_OWNER_FREE);
    drop_launcher(l);
}

/* A started program is no longer a hold: its keyboard is not the launcher's
 * to change any more. */
static void test_started_context_cannot_be_rebound(void)
{
    kbd_source_reset();
    size_t total = 0;
    uint64_t paddr = map_package(&total);
    page_owner_t l = make_launcher();
    page_owner_t c = hold_bind_start(total, l, EXO_KBD_SRC_PS2);

    CU_ASSERT_EQUAL(do_bind(c, EXO_KBD_SRC_NONE), -EXO_ENOENT);
    CU_ASSERT_EQUAL(kbd_source_owner(KBD_SRC_PS2), c);   /* still bound */

    revoke_all(c);
    context_destroy(c);
    drop_launcher(l);
    unmap_package(paddr);
}

static void test_bad_and_missing_sources(void)
{
    kbd_source_reset();
    size_t total = 0;
    uint64_t paddr = map_package(&total);
    page_owner_t l = make_launcher();
    page_owner_t c = hold(total);

    /* Not a source number at all. */
    CU_ASSERT_EQUAL(do_bind(c, KBD_SRC_MAX), -EXO_EINVAL);
    CU_ASSERT_EQUAL(do_bind(c, 1ull << 40), -EXO_EINVAL);
    CU_ASSERT_EQUAL(do_bind(c, (uint64_t)-2), -EXO_EINVAL);
    /* A valid number with no keyboard behind it right now. */
    CU_ASSERT_EQUAL(do_bind(c, EXO_KBD_SRC_USB(usb_kbd_count())), -EXO_ENOENT);
    CU_ASSERT_EQUAL(kbd_source_of(c), KBD_SRC_NONE);

    revoke_all(c);
    context_destroy(c);
    drop_launcher(l);
    unmap_package(paddr);
}

/* A context nobody bound keeps reading PS/2, as before #44 existed. */
static void test_unbound_context_still_reads_ps2(void)
{
    kbd_source_reset();
    kbd_reset();
    kbd_event_t k = { .pressed = 1, .key = (uint8_t)KEY_Q, .modifiers = 0 };
    kbd_enqueue(k);
    kbd_event_t ev;
    CU_ASSERT_EQUAL(kbd_source_poll(PAGE_OWNER_LIBOS, &ev), 1);
    CU_ASSERT_EQUAL(ev.key, (uint8_t)KEY_Q);
}

void suite_syscall_kbd_bind_tests(CU_pSuite s)
{
    CU_add_test(s, "handler is bound", test_handler_is_bound);
    CU_add_test(s, "two contexts read only their own keyboard",
                test_two_contexts_read_only_their_own_keyboard);
    CU_add_test(s, "binding a source twice is EBUSY, first survives",
                test_binding_twice_is_busy_and_first_survives);
    CU_add_test(s, "unbind frees the source", test_unbind_frees_the_source);
    CU_add_test(s, "dropped, revoked and exited contexts free the source",
                test_dropped_revoked_and_exited_contexts_free_the_source);
    CU_add_test(s, "only the launcher may bind its hold",
                test_only_the_launcher_may_bind_its_hold);
    CU_add_test(s, "not a hold is ENOENT", test_not_a_hold_is_enoent);
    CU_add_test(s, "a started context cannot be rebound",
                test_started_context_cannot_be_rebound);
    CU_add_test(s, "bad and missing sources", test_bad_and_missing_sources);
    CU_add_test(s, "unbound context still reads PS/2",
                test_unbound_context_still_reads_ps2);
}
