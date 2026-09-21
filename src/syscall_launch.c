/*
 * syscall_launch.c — EXO_SYS_LAUNCH (#21), the one syscall that starts a
 * ring-3 LibOS app (SCRUM-184).
 *
 * Before this ticket every launchable app had a syscall number of its own --
 * EXO_SYS_LAUNCH_WAD_VIEWER (#21, SCRUM-178), _CLOCK (#22, SCRUM-168),
 * _SNAKE (#23, SCRUM-182) and _DOOM (#24, SCRUM-66) -- each with a
 * hand-written sys_launch_*() closing over its own build-time blob symbols,
 * and each costing an edit to exo_syscall.h's dense table plus the two tests
 * that police it. Four apps in, the four handlers differed only in which
 * blobs they named and which of two optional steps they ran, while the other
 * forty-odd lines were copied verbatim.
 *
 * So the per-app part is now DATA -- one row in launch_apps[] naming the
 * blobs, the bss length and two flags -- and the procedure is written once,
 * below. Adding an app is a row plus a shell command; it is no longer a
 * syscall number, a dispatcher entry, and test churn.
 *
 * ── What a row has to say ─────────────────────────────────────────────
 *
 * Only two things actually varied between the old handlers, and both are
 * flags rather than code:
 *
 *   LAUNCH_NEEDS_WAD    map the multiboot WAD module into the new address
 *                       space and patch its address/length into the image's
 *                       params page. The WAD viewer and Doom need it; the
 *                       clock and Snake have no external resource to stage.
 *
 *   LAUNCH_RELEASES_FB  release whoever currently holds the framebuffer
 *                       binding before entering.
 *
 * That second flag preserves an inconsistency rather than inventing one,
 * which is worth being explicit about since it is exactly the sort of thing
 * a refactor quietly "tidies away": sys_launch_wad_viewer() and
 * sys_launch_snake() both called fb_binding_release(), and
 * sys_launch_clock() deliberately did not -- its comment argued the call is
 * unnecessary under framebuffer multiplexing (SCRUM-112, src/fb_shadow.c),
 * since every app gets its own private virtual framebuffer regardless of who
 * else is live. That argument looks right, and would apply equally to the
 * other three. But "looks right" is not "tested", and changing three apps'
 * launch behaviour is not what this ticket was asked to do. The flag keeps
 * each app doing exactly what it did before and makes the discrepancy
 * visible in one table instead of buried across four functions. Resolving it
 * is its own change.
 */

#include "syscall_launch.h"
#include "syscall.h"
#include "exo_syscall.h"
#include "context.h"
#include "libos_launch.h"
#include "libos_wad_map.h"
#include "libos_wad_params.h"
#include "libos_wad_viewer/libos_wad_viewer_layout.h"
#include "libos_clock/libos_clock_layout.h"
#include "libos_snake/libos_snake_layout.h"
#include "libos_doom/libos_doom_layout.h"
#include "mmap.h"
#include "page_alloc.h"
#include "vmm.h"
#include "fb_binding.h"
#include "revoke.h"

#include <stddef.h>
#include <stdint.h>

/* Embedded code/data blobs -- produced at build time by
 * build_ring3_link_target's objcopy step (docker/scripts/build.sh), the same
 * mechanism as the shell LibOS's own _binary_shell_*_bin_* symbols. */
extern const uint8_t _binary_libos_wad_viewer_code_bin_start[];
extern const uint8_t _binary_libos_wad_viewer_code_bin_end[];
extern const uint8_t _binary_libos_wad_viewer_data_bin_start[];
extern const uint8_t _binary_libos_wad_viewer_data_bin_end[];

extern const uint8_t _binary_libos_clock_code_bin_start[];
extern const uint8_t _binary_libos_clock_code_bin_end[];
extern const uint8_t _binary_libos_clock_data_bin_start[];
extern const uint8_t _binary_libos_clock_data_bin_end[];

extern const uint8_t _binary_libos_snake_code_bin_start[];
extern const uint8_t _binary_libos_snake_code_bin_end[];
extern const uint8_t _binary_libos_snake_data_bin_start[];
extern const uint8_t _binary_libos_snake_data_bin_end[];

/* Doom's blobs (SCRUM-66), same mechanism. Much the largest of the four:
 * ~390 KiB of code+rodata and ~80 KiB of .data against a ~314 KiB .bss,
 * which is why LIBOS_LAUNCH_MAX_{CODE,DATA}_PAGES had to grow from 8/16 to
 * 192/192 in src/libos_launch.h for this ticket. */
extern const uint8_t _binary_libos_doom_code_bin_start[];
extern const uint8_t _binary_libos_doom_code_bin_end[];
extern const uint8_t _binary_libos_doom_data_bin_start[];
extern const uint8_t _binary_libos_doom_data_bin_end[];

#define LAUNCH_NEEDS_WAD    (1u << 0)
#define LAUNCH_RELEASES_FB  (1u << 1)

typedef struct {
    const uint8_t *code_start;
    const uint8_t *code_end;
    const uint8_t *data_start;
    const uint8_t *data_end;
    size_t         bss_len;
    unsigned       flags;

    /*
     * The most recently launched instance's context id, or PAGE_OWNER_FREE
     * if none is live. Per-app and mutable, which is why launch_apps[] is
     * not const.
     *
     * Each app exits via exo_exit() (#20, src/syscall_exit.c,
     * SCRUM-155/178), which reclaims its pages and framebuffer binding
     * immediately and hands off to the shell -- but it cannot
     * context_destroy() its own context_t row, because
     * context_switch_request() inside exo_exit() still needs that row live,
     * as the outgoing side, to capture into. The row is therefore left
     * behind READY but resourceless, and without reclaiming it here a second
     * launch of the same app would context_create() yet another context
     * alongside the shell and the leftover row, eventually exhausting
     * CONTEXT_MAX.
     *
     * revoke_all() is a harmless no-op by the time the reclaim runs
     * (exo_exit() already freed everything); context_destroy() is the part
     * that still matters, to free the table slot itself. Kept as a pair
     * regardless, in case an app ever exits some other way -- a crash, or a
     * future teardown path exo_exit() does not cover -- that leaves real
     * resources behind after all.
     *
     * Per-app rather than one shared "the current app": under framebuffer
     * multiplexing (SCRUM-112) these are independent live contexts, not
     * mutually exclusive the way "the current viewer" was before it.
     */
    page_owner_t   last_id;
} launch_app_t;

/*
 * Indexed by EXO_LAUNCH_APP_* (src/exo_syscall.h). That order is ABI -- the
 * ids cross the syscall boundary -- so rows may be appended but not
 * reordered, and EXO_LAUNCH_APP_COUNT stays one past the last.
 */
static launch_app_t launch_apps[EXO_LAUNCH_APP_COUNT] = {
    [EXO_LAUNCH_APP_WAD_VIEWER] = {
        _binary_libos_wad_viewer_code_bin_start,
        _binary_libos_wad_viewer_code_bin_end,
        _binary_libos_wad_viewer_data_bin_start,
        _binary_libos_wad_viewer_data_bin_end,
        LIBOS_WAD_VIEWER_BSS_LEN,
        LAUNCH_NEEDS_WAD | LAUNCH_RELEASES_FB,
        PAGE_OWNER_FREE,
    },
    [EXO_LAUNCH_APP_CLOCK] = {
        _binary_libos_clock_code_bin_start,
        _binary_libos_clock_code_bin_end,
        _binary_libos_clock_data_bin_start,
        _binary_libos_clock_data_bin_end,
        LIBOS_CLOCK_BSS_LEN,
        0u,
        PAGE_OWNER_FREE,
    },
    [EXO_LAUNCH_APP_SNAKE] = {
        _binary_libos_snake_code_bin_start,
        _binary_libos_snake_code_bin_end,
        _binary_libos_snake_data_bin_start,
        _binary_libos_snake_data_bin_end,
        LIBOS_SNAKE_BSS_LEN,
        LAUNCH_RELEASES_FB,
        PAGE_OWNER_FREE,
    },
    [EXO_LAUNCH_APP_DOOM] = {
        _binary_libos_doom_code_bin_start,
        _binary_libos_doom_code_bin_end,
        _binary_libos_doom_data_bin_start,
        _binary_libos_doom_data_bin_end,
        LIBOS_DOOM_BSS_LEN,
        LAUNCH_NEEDS_WAD | LAUNCH_RELEASES_FB,
        PAGE_OWNER_FREE,
    },
};

/*
 * #21 -- build and switch to the ring-3 LibOS app named by `app_id`,
 * invoked from the shell's per-app commands (src/shell/shell_main.c).
 *
 * Returns a negative EXO_E* on any failure before the switch is armed, in
 * which case the caller (the shell) keeps running and can report the error.
 * On success, context_switch_request() has armed context_switch_pending and
 * this returns 0 -- but the caller never observes that 0 as an ordinary
 * syscall return: src/syscall_entry.s's epilogue sees the pending switch
 * immediately after this handler returns and takes the context_switch_tail
 * exit into the new app instead of sysret-ing back to the shell.
 */
static int64_t sys_launch(uint64_t app_id, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    /* The one new failure mode this ticket introduces. An unknown app id is
     * a caller error rather than a resource problem, and is rejected before
     * anything has been created. The argument is unsigned, so a "negative"
     * id arrives as a huge value and is caught by the same comparison. */
    if (app_id >= EXO_LAUNCH_APP_COUNT) {
        return -EXO_EINVAL;
    }

    launch_app_t *app = &launch_apps[app_id];

    /* Reclaim the previous instance of THIS app before creating another --
     * see last_id's own comment. context_lookup() guards against a stale id
     * from an instance some other path already tore down. */
    if (app->last_id != PAGE_OWNER_FREE &&
        context_lookup(app->last_id) != NULL) {
        revoke_all(app->last_id);
        context_destroy(app->last_id);
    }
    app->last_id = PAGE_OWNER_FREE;

    /* Looked up before context_create() so a missing module fails without
     * having allocated anything -- the ordering the old WAD-viewer handler
     * used, preserved. */
    uint64_t wad_start = 0, wad_end = 0, wad_size = 0;
    if (app->flags & LAUNCH_NEEDS_WAD) {
        if (mmap_find_module(&wad_start, &wad_end) != 0) {
            return -EXO_ENODEV;
        }
        wad_size = wad_end - wad_start;
    }

    page_owner_t id;
    int create_rc = context_create(vmm_kernel_pml4(), &id);
    if (create_rc != CONTEXT_OK) {
        return create_rc == CONTEXT_ENOMEM ? -EXO_ENOMEM : -EXO_EINVAL;
    }

    size_t code_len = (size_t)(app->code_end - app->code_start);
    size_t data_len = (size_t)(app->data_end - app->data_start);

    /* static, not a local: libos_image_t carries one uint64_t per mappable
     * page, so at 192/192/16 pages it is a little over 3 KiB -- a fifth of
     * src/syscall_entry.s's 16 KiB syscall stack to spend on one local.
     * Safe because a launch cannot overlap another: `syscall` masks IF
     * (FMASK, src/syscall.c) so no interrupt can re-enter the dispatcher
     * here, and this handler runs to completion before the switch it arms
     * ever takes effect. Introduced by SCRUM-66 for Doom, and now shared by
     * every app, since there is only one handler. */
    static libos_image_t img;
    if (libos_build_image(id, app->code_start, code_len,
                          app->data_start, data_len,
                          app->bss_len, &img) != VMM_OK) {
        context_destroy(id);
        return -EXO_ENOMEM;
    }

    if (app->flags & LAUNCH_NEEDS_WAD) {
        if (libos_map_wad((uint64_t *)(uintptr_t)img.pml4_phys,
                          wad_start, wad_size) != VMM_OK) {
            libos_destroy_image(id, &img);
            context_destroy(id);
            return -EXO_EINVAL;
        }

        /* SCRUM-175: patch libos_wad_params_t -- the only thing the app
         * cannot learn through an ordinary syscall (see
         * src/libos_wad_params.h) -- directly into the app's own first
         * .data global, rather than hand-mapping a separate side-channel
         * page the way an earlier version of this mechanism did. See
         * libos_launch_patch_params()'s own comment (src/libos_launch.h) for
         * why that is safe: it requires only that the TU owning that global
         * stays first in build.sh's source list for the target. Unlike the
         * old side-channel page (mapped read-only), the global lives in
         * ordinary writable .data, so the app itself could in principle
         * corrupt its own wad_vaddr/wad_size after launch -- an accepted
         * widening, not a regression the kernel needs to guard against, per
         * that same comment. */
        libos_wad_params_t params = {
            .wad_vaddr = LIBOS_WAD_VADDR,
            .wad_size  = wad_size,
        };
        if (libos_launch_patch_params(&img, &params, sizeof(params)) != VMM_OK) {
            libos_destroy_image(id, &img);
            context_destroy(id);
            return -EXO_EINVAL;
        }
    }

    /* _irq: this is the app's very first entry, reached through
     * context_switch_tail rather than a direct libos_enter_irq() call --
     * without it the app would launch with RFLAGS.IF clear (context_prime()'s
     * default, correct for SCRUM-108/109's cooperative-only ping-pong
     * probes) and its exo_get_ticks()/exo_kbd_poll()-driven loops would never
     * see a PIT or keyboard IRQ land. */
    context_prime_irq(id, img.entry_vaddr, img.stack_top_vaddr);

    if (app->flags & LAUNCH_RELEASES_FB) {
        /* Whoever currently holds the framebuffer binding -- the shell, from
         * its own libos_fb_map() call at startup (src/shell/shell_main.c),
         * on a first launch; nobody, on a later one, since the reclaim at
         * the top of this function already released it from the previous
         * instance -- must give it up before this one can exo_fb_acquire()
         * it (exclusive, src/fb_binding.c, SCRUM-154). fb_binding_owner()
         * rather than assuming the caller: the caller is always the shell,
         * but the *holder* is not, once a second launch runs. Only the
         * binding is released, not the holder's own already-established page
         * table mapping (fb_binding_release() never touches page tables), so
         * the shell keeps rendering to its existing framebuffer pointer
         * uninterrupted once it is switched back to. Done last, right before
         * the point of no return: every earlier failure path above leaves
         * whoever holds the binding untouched. */
        fb_binding_release(fb_binding_owner());
    }

    /* Succeeds because the caller (the shell) has a real context_t row of
     * its own -- see src/kernel.c's SCRUM-178 refactor of the shell launch.
     * Arms context_switch_pending; src/syscall_entry.s does the actual
     * switch once this handler returns. */
    if (context_switch_request(id) != CONTEXT_OK) {
        libos_destroy_image(id, &img);
        context_destroy(id);
        return -EXO_EINVAL;
    }

    /* Only now, on the success path -- everything above that fails instead
     * destroys `id` itself and returns with app->last_id still
     * PAGE_OWNER_FREE from the top of this function. */
    app->last_id = id;

    return 0;
}

void syscall_launch_init(void)
{
    exo_syscall_register(EXO_SYS_LAUNCH, sys_launch);
}
