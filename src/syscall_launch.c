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
 *   LAUNCH_NEEDS_WAD    map the IWAD into the new address space and patch
 *                       its address/length into the image's params page.
 *                       The WAD viewer and Doom need it; the clock and
 *                       Snake have no external resource to stage. The IWAD
 *                       itself arrives via ExoFS/ATA, cached after its
 *                       first disk read (SCRUM-190, doom_wad_disk_get()
 *                       below) -- it used to be a GRUB multiboot module
 *                       mapped in before the kernel ever ran, which is
 *                       where "the multiboot WAD module" in some of the
 *                       comments below still refers to.
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
#include "exo_pkg.h"
#include "exo_volume.h"
#include "disk_binding.h"
#include "libos_wad_map.h"
#include "libos_wad_params.h"
#include "libos_wad_viewer/libos_wad_viewer_layout.h"
#include "libos_doom/libos_doom_layout.h"
#include "doom_wad_disk.h"
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
    },
    [EXO_LAUNCH_APP_DOOM] = {
        _binary_libos_doom_code_bin_start,
        _binary_libos_doom_code_bin_end,
        _binary_libos_doom_data_bin_start,
        _binary_libos_doom_data_bin_end,
        LIBOS_DOOM_BSS_LEN,
        LAUNCH_NEEDS_WAD | LAUNCH_RELEASES_FB,
    },
};

/*
 * The one context #36 has built under EXO_LAUNCH_HOLD and not yet entered
 * (SCRUM-235). One slot, not a table: the only caller is the shell's `run`,
 * which holds, reads the grant and starts in one go. A second hold, or any
 * other launch, first drops a hold that was never started, so an abandoned
 * `run` cannot strand a context row and its pages.
 */
static struct {
    page_owner_t id;          /* PAGE_OWNER_FREE when nothing is held */
    int          release_fb;  /* the manifest's EXO_PKG_WANT_FRAMEBUFFER */
} held;

/* A hold is only still a hold while its context is the READY, never-run row
 * it was built as. If something else already took it -- it was switched to by
 * a round-robin yield and ran or exited, or its row is gone -- the slot is
 * stale and its id may by now belong to another context, which must not be
 * torn down on the strength of an old hold. */
static int launch_held_is_live(void)
{
    if (held.id == PAGE_OWNER_FREE) {
        return 0;
    }
    context_t *c = context_lookup(held.id);
    return c != NULL && c->state == CONTEXT_STATE_READY;
}

static void launch_drop_held(void)
{
    if (launch_held_is_live()) {
        revoke_all(held.id);
        context_destroy(held.id);
    }
    held.id = PAGE_OWNER_FREE;
}

/*
 * Shared by #21 and #36: both create a context row, and both would exhaust
 * CONTEXT_MAX if an exited app's row were never reclaimed.
 */
static void launch_reclaim_exited(void)
{
    launch_drop_held();

    /*
     * Reclaim any exited-but-not-yet-destroyed context row before creating
     * another one. exo_exit() (src/syscall_exit.c, SCRUM-155/178) already
     * frees a terminating context's pages/bindings via reclaim_pages_owned(),
     * but cannot context_destroy() its own row -- context_switch_request()
     * inside exo_exit() still needs that row live, as the outgoing side, to
     * capture into -- so it leaves the row behind, downgraded to
     * CONTEXT_STATE_BLOCKED (SCRUM-111) precisely so context_next_ready()'s
     * round-robin skips it. Nothing else destroys it. Before this ticket that
     * fell out of sys_launch()'s own last_id-keyed "tear down my previous
     * instance" step; now that concurrent instances of the same app are
     * allowed (SCRUM-196/SCRUM-195), that step is gone, so the sweep has to
     * be table-wide and app-agnostic instead of per-app, or a BLOCKED row
     * from any app would simply never get reclaimed and CONTEXT_MAX (14)
     * would eventually exhaust. revoke_all() is a harmless no-op by the time
     * this runs (exo_exit() already freed everything); context_destroy() is
     * the part that still matters, to free the table slot itself. Kept as a
     * pair regardless, in case an app ever exits some other way -- a crash,
     * or a future teardown path exo_exit() does not cover -- that leaves
     * real resources behind after all.
     */
    context_info_t live[CONTEXT_MAX];
    uint32_t live_count = context_list(live, CONTEXT_MAX);
    for (uint32_t i = 0; i < live_count; i++) {
        if (live[i].state == CONTEXT_STATE_BLOCKED) {
            revoke_all(live[i].id);
            context_destroy(live[i].id);
        }
    }
}

/* static, not a local: libos_image_t carries one uint64_t per mappable
 * page, so at 192/192/16 pages it is a little over 3 KiB -- a fifth of
 * src/syscall_entry.s's 16 KiB syscall stack to spend on one local.
 * Safe because a launch cannot overlap another: `syscall` masks IF
 * (FMASK, src/syscall.c) so no interrupt can re-enter the dispatcher
 * here, and a handler runs to completion before the switch it arms
 * ever takes effect. Introduced by SCRUM-66 for Doom; shared by #21 and #36
 * (SCRUM-230), which is safe for the same reason. */
static libos_image_t launch_img;

/* The framebuffer half of a launch whose manifest asks for the binding.
 * Shared by launch_enter() and #40 so the two cannot drift. */
static void launch_release_fb(void)
{
    /* Whoever currently holds the framebuffer binding -- the shell, from
     * its own libos_fb_map() call at startup (src/shell/shell_main.c),
     * on a first launch; some other still-live app, once more than one
     * instance can be running at once (SCRUM-196) -- must give it up
     * before this one can exo_fb_acquire() it (exclusive,
     * src/fb_binding.c, SCRUM-154). fb_binding_owner()
     * rather than assuming the caller: the caller is always the shell,
     * but the *holder* is not, once a second launch runs. Only the
     * binding is released, not the holder's own already-established page
     * table mapping (fb_binding_release() never touches page tables), so
     * the shell keeps rendering to its existing framebuffer pointer
     * uninterrupted once it is switched back to. Callers run it only once
     * the switch is armed: every failure path before that leaves whoever
     * holds the binding untouched.
     *
     * KNOWN GAP: this displaces the holder whoever the
     * caller is -- the shell displacing a still-running Snake is the
     * designed case (SCRUM-196), so "caller must be the holder" is not
     * available. #21 always had this reach; #36 only adds that the
     * package, not a kernel table, supplies the flag. SCRUM-232's
     * admission check does not close it: it reports a held framebuffer
     * (exo_resstat.fb_held) so the launcher can say it is taking it over,
     * but telling a launcher from an arbitrary LibOS is still open.
     */
    fb_binding_release(fb_binding_owner());
}

/*
 * The part of a launch that does not care where the image's bytes came from
 * (shared by #21 and #36, so the two cannot drift): prime the context,
 * optionally release the framebuffer binding, arm the switch. On failure
 * tears down the image and the context row, so the caller only has to
 * return the error.
 */
static int64_t launch_enter(page_owner_t id, const libos_image_t *img,
                            int release_fb)
{
    /* _irq: this is the app's very first entry, reached through
     * context_switch_tail rather than a direct libos_enter_irq() call --
     * without it the app would launch with RFLAGS.IF clear (context_prime()'s
     * default, correct for SCRUM-108/109's cooperative-only ping-pong
     * probes) and its exo_get_ticks()/exo_kbd_poll()-driven loops would never
     * see a PIT or keyboard IRQ land. */
    context_prime_irq(id, img->entry_vaddr, img->stack_top_vaddr);

    /* Succeeds because the caller (the shell) has a real context_t row of
     * its own -- see src/kernel.c's SCRUM-178 refactor of the shell launch.
     * Arms context_switch_pending; src/syscall_entry.s does the actual
     * switch once this handler returns. This is the last step that can
     * fail, so it runs BEFORE the framebuffer release below: a refused
     * launch must leave whoever holds the binding untouched (SCRUM-230). */
    if (context_switch_request(id) != CONTEXT_OK) {
        libos_destroy_image(id, img);
        /* Drop a volume assigned by #36 (SCRUM-237): the id is about to be
         * free for reuse and must not inherit another program's extent. */
        disk_binding_forget(id);
        context_destroy(id);
        return -EXO_EINVAL;
    }

    if (release_fb) {
        launch_release_fb();
    }

    return 0;
}

/*
 * Map the shared WAD buffer into a freshly built image and patch its launch
 * params (shared by #21's LAUNCH_NEEDS_WAD rows and #36's EXO_PKG_WANT_WAD,
 * SCRUM-232, so the two cannot drift). On failure tears down the image and
 * the context row, like launch_enter(), so the caller only returns the error.
 *
 * wad_paddr/wad_size name the shared, cached buffer doom_wad_disk_get()
 * returned -- never this launch's alone -- so a failure here tears down this
 * app's own image/context but must not free those pages; some other
 * still-live app (or a future launch) may be mapping the same buffer.
 *
 * SCRUM-175: libos_wad_params_t -- the only thing the app cannot learn
 * through an ordinary syscall (see src/libos_wad_params.h) -- is patched
 * directly into the app's own first .data global. See
 * libos_launch_patch_params()'s comment (src/libos_launch.h) for why that is
 * safe: it requires only that the TU owning that global stays first in
 * build.sh's source list for the target. The global lives in ordinary
 * writable .data, so the app could corrupt its own wad_vaddr/wad_size after
 * launch -- an accepted widening, not something the kernel needs to guard.
 */
static int64_t launch_attach_wad(page_owner_t id, libos_image_t *img,
                                 uint64_t wad_paddr, uint64_t wad_size)
{
    if (libos_map_wad((uint64_t *)(uintptr_t)img->pml4_phys,
                      wad_paddr, wad_size) != VMM_OK) {
        libos_destroy_image(id, img);
        disk_binding_forget(id); /* a volume #36 assigned (SCRUM-237) */
        context_destroy(id);
        return -EXO_EINVAL;
    }

    libos_wad_params_t params = {
        .wad_vaddr = LIBOS_WAD_VADDR,
        .wad_size  = wad_size,
    };
    if (libos_launch_patch_params(img, &params, sizeof(params)) != VMM_OK) {
        libos_destroy_image(id, img);
        disk_binding_forget(id); /* a volume #36 assigned (SCRUM-237) */
        context_destroy(id);
        return -EXO_EINVAL;
    }
    return 0;
}

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

    launch_reclaim_exited();

    /* Looked up before context_create() so a missing/unreadable WAD fails
     * without having allocated anything -- the ordering the old WAD-viewer
     * handler used for the GRUB module lookup, preserved for the disk-backed
     * one (SCRUM-190). */
    uint64_t wad_paddr = 0, wad_size = 0;
    if (app->flags & LAUNCH_NEEDS_WAD) {
        int64_t wad_rc = doom_wad_disk_get(&wad_paddr, &wad_size);
        if (wad_rc != 0) {
            return wad_rc;
        }
    }

    page_owner_t id;
    int create_rc = context_create(vmm_kernel_pml4(), &id);
    if (create_rc != CONTEXT_OK) {
        return create_rc == CONTEXT_ENOMEM ? -EXO_ENOMEM : -EXO_EINVAL;
    }

    size_t code_len = (size_t)(app->code_end - app->code_start);
    size_t data_len = (size_t)(app->data_end - app->data_start);

    if (libos_build_image(id, app->code_start, code_len,
                          app->data_start, data_len,
                          app->bss_len, &launch_img) != VMM_OK) {
        context_destroy(id);
        return -EXO_ENOMEM;
    }

    if (app->flags & LAUNCH_NEEDS_WAD) {
        int64_t wrc = launch_attach_wad(id, &launch_img, wad_paddr, wad_size);
        if (wrc != 0) {
            return wrc;
        }
    }

    return launch_enter(id, &launch_img, app->flags & LAUNCH_RELEASES_FB);
}

/* EXO_LAUNCH_PKG_MAX_LEN (src/exo_syscall.h) restates the format's worst case
 * as plain integers because that header cannot include exo_pkg.h; this is
 * what keeps the two from drifting. */
_Static_assert(EXO_LAUNCH_PKG_MAX_LEN ==
               sizeof(exo_pkg_header_t) +
               (EXO_PKG_MAX_CODE_PAGES + EXO_PKG_MAX_DATA_PAGES) *
               EXO_PKG_PAGE_SIZE,
               "EXO_LAUNCH_PKG_MAX_LEN out of sync with src/exo_pkg.h");

/*
 * #36 -- build and switch to the LibOS packaged at [pkg, pkg + len)
 * (SCRUM-230). The package is the on-disk format of src/exo_pkg.h; the
 * caller -- a ring-3 launcher, today the shell -- read it off an ExoFS volume
 * itself, because the kernel knows sectors and nothing above them
 * (docs/syscall_spec.md §3.2 #24). Everything past "where did the bytes come
 * from" is #21's path, unchanged: libos_build_image() copies the sections
 * into pages the new context owns, and launch_enter() arms the switch.
 *
 * Validation order is shape -> buffer -> contents, the precedence §3.5a
 * settled on: a length that cannot be a package is rejected before the
 * pointer is walked, and the pointer is checked before any byte behind it is
 * read (SCRUM-186: an in-window but unmapped read faults the kernel).
 *
 * The package is untrusted: exo_pkg_parse() bounds every length against the
 * loader's caps and copies the header out by value, so nothing below re-reads
 * a header field from caller memory. Only code/data bytes are read from the
 * buffer again, and libos_build_image() copies them exactly once.
 */
static int64_t sys_launch_pkg(uint64_t pkg, uint64_t len, uint64_t flags,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;

    /* Shape first, like the length: a flag this kernel does not know is a
     * caller that expects behavior it will not get. */
    if ((flags & ~(uint64_t)EXO_LAUNCH_HOLD) != 0) {
        return -EXO_EINVAL;
    }
    if (len < sizeof(exo_pkg_header_t) || len > EXO_LAUNCH_PKG_MAX_LEN) {
        return -EXO_EINVAL;
    }
    if (!exo_range_in_user_window(pkg, len) ||
        !exo_user_range_mapped(pkg, len, 0)) {
        return -EXO_EFAULT;
    }

    exo_pkg_header_t hdr;
    const void *code, *data;
    if (exo_pkg_parse((const void *)(uintptr_t)pkg, (size_t)len,
                      &hdr, &code, &data) != 0) {
        return -EXO_EINVAL;
    }
    /* The header can declare a params struct (first global in .data) for the
     * launcher to patch, but no patch is wired for packages yet -- Doom and
     * the WAD viewer still launch through #21. Launching such a package
     * would start it with a zero-filled params struct and let it misbehave
     * later, so refuse it now rather than run it half-configured. */
    int wants_wad = (hdr.want_flags & EXO_PKG_WANT_WAD) != 0;
    if (wants_wad ? hdr.params_len != sizeof(libos_wad_params_t)
                  : hdr.params_len != 0) {
        return -EXO_EINVAL;
    }
    /* A package with no code has nothing for entry_vaddr to point at;
     * libos_build_image() would refuse it, but refuse it before a context
     * row exists rather than after. */
    if (hdr.code_len == 0) {
        return -EXO_EINVAL;
    }

    launch_reclaim_exited();

    /* Looked up before anything is allocated, same ordering as #21: a
     * missing or unreadable WAD fails without a context row or a page taken. */
    uint64_t wad_paddr = 0, wad_size = 0;
    if (wants_wad) {
        int64_t wad_rc = doom_wad_disk_get(&wad_paddr, &wad_size);
        if (wad_rc != 0) {
            return wad_rc;
        }
    }

    /* Backstop, not policy (SCRUM-232): whether a program fits is the
     * launcher's call, made from exo_resstat (#38) and the manifest. This is
     * only here so a launcher that skips that check gets a clean -ENOMEM
     * before anything is allocated, rather than a half-built image failing
     * inside libos_build_image(). The manifest is the package's own claim, so
     * it is a floor on what the kernel will try, never a promise. */
    uint32_t total_pages, free_pages, kernel_pages, libos_pages;
    page_alloc_totals(&total_pages, &free_pages, &kernel_pages, &libos_pages);
    if (free_pages < hdr.min_pages) {
        return -EXO_ENOMEM;
    }

    page_owner_t id;
    int create_rc = context_create(vmm_kernel_pml4(), &id);
    if (create_rc != CONTEXT_OK) {
        return create_rc == CONTEXT_ENOMEM ? -EXO_ENOMEM : -EXO_EINVAL;
    }

    if (libos_build_image(id, code, hdr.code_len, data, hdr.data_len,
                          hdr.bss_len, &launch_img) != VMM_OK) {
        context_destroy(id);
        return -EXO_ENOMEM;
    }

    /* The manifest's volume request becomes a sector extent the program alone
     * may bind (SCRUM-237). Done after the image exists but before the
     * program runs, and undone with the context if it cannot be granted --
     * another running program already holds that slot, say. */
    if (hdr.volume_sectors != 0) {
        int arc = disk_binding_assign(id, exo_volume_slot_base(hdr.volume_slot),
                                      hdr.volume_sectors);
        if (arc != DISK_BIND_OK) {
            revoke_all(id);
            context_destroy(id);
            return arc == DISK_BIND_EBUSY ? -EXO_EBUSY
                 : arc == DISK_BIND_ENODEV ? -EXO_ENODEV : -EXO_EINVAL;
        }
    }

    if (wants_wad) {
        int64_t wrc = launch_attach_wad(id, &launch_img, wad_paddr, wad_size);
        if (wrc != 0) {
            return wrc;
        }
    }

    /* libos_build_image() maps LIBOS_LAUNCH_STACK_PAGES regardless, so the
     * header's stack_pages is advisory until the builder takes a count. */

    /* The manifest, not a table flag, says whether this program wants the
     * framebuffer: the same release #21 does for Snake/Doom/the viewer. */
    int release_fb = (hdr.want_flags & EXO_PKG_WANT_FRAMEBUFFER) != 0;

    if (flags & EXO_LAUNCH_HOLD) {
        /* Built and primed, not entered (SCRUM-235): the launcher reads the
         * grant with #39 and then calls #40. Nothing is released yet --
         * the framebuffer binding moves in #40, after the switch is armed,
         * so a hold that is never started displaces nobody. */
        context_prime_irq(id, launch_img.entry_vaddr, launch_img.stack_top_vaddr);
        held.id = id;
        held.release_fb = release_fb;
        return (int64_t)id;
    }

    return launch_enter(id, &launch_img, release_fb);
}

/* #40 -- enter the context #36 built under EXO_LAUNCH_HOLD (SCRUM-235).
 * The other half of launch_enter(): same switch, same framebuffer release
 * after it, but the image already exists. Like #21/#36, control does not
 * come back until something switches back to the caller.
 *   -EXO_ENOENT   `ctx` is not the held context
 *   -EXO_EINVAL   the switch could not be armed; the held context is gone */
static int64_t sys_launch_start(uint64_t ctx, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (!launch_held_is_live() || ctx != held.id) {
        return -EXO_ENOENT;
    }

    page_owner_t id = held.id;
    int release_fb = held.release_fb;
    held.id = PAGE_OWNER_FREE;

    if (context_switch_request(id) != CONTEXT_OK) {
        /* revoke_all() frees the image's pages and the volume extent the
         * way launch_drop_held() does; the image struct is long gone. */
        revoke_all(id);
        disk_binding_forget(id);
        context_destroy(id);
        return -EXO_EINVAL;
    }
    if (release_fb) {
        launch_release_fb();
    }
    return 0;
}

void syscall_launch_init(void)
{
    exo_syscall_register(EXO_SYS_LAUNCH, sys_launch);
    exo_syscall_register(EXO_SYS_LAUNCH_PKG, sys_launch_pkg);
    exo_syscall_register(EXO_SYS_LAUNCH_START, sys_launch_start);
}
