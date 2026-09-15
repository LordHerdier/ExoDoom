#include "syscall_launch.h"
#include "syscall.h"
#include "exo_syscall.h"
#include "context.h"
#include "libos_launch.h"
#include "libos_wad_map.h"
#include "libos_wad_params.h"
#include "libos_wad_viewer/libos_wad_viewer_layout.h"
#include "mmap.h"
#include "page_alloc.h"
#include "vmm.h"
#include "fb_binding.h"
#include "revoke.h"

#include <stddef.h>
#include <stdint.h>

/* Embedded WAD viewer code/data blobs -- produced at build time by
 * build_ring3_link_target's objcopy step (docker/scripts/build.sh), the same
 * mechanism as the shell LibOS's own _binary_shell_*_bin_* symbols
 * (src/kernel.c). LIBOS_WAD_VIEWER_BSS_LEN comes from that step's generated
 * src/libos_wad_viewer/libos_wad_viewer_layout.h. */
extern const uint8_t _binary_libos_wad_viewer_code_bin_start[];
extern const uint8_t _binary_libos_wad_viewer_code_bin_end[];
extern const uint8_t _binary_libos_wad_viewer_data_bin_start[];
extern const uint8_t _binary_libos_wad_viewer_data_bin_end[];

/* The most recently launched viewer's context id, or PAGE_OWNER_FREE if
 * none is live. `wadview` now exits via exo_exit() on Q/Esc (#20,
 * src/syscall_exit.c, SCRUM-155/178), which reclaims its pages and
 * framebuffer binding immediately and hands off to the shell -- but it
 * cannot context_destroy() its own context_t row (context_switch_request()
 * inside exo_exit() still needs to find that row live, as the outgoing
 * side, to capture into), so the row itself is left behind, READY but
 * resourceless. Without reclaiming it here, a second `wadview` would
 * context_create() a *third* context alongside the shell and the first
 * viewer's leftover row, eventually exhausting CONTEXT_MAX (3). revoke_all()
 * below is a harmless no-op by the time this runs (exo_exit() already freed
 * everything); context_destroy() is the part that still matters, to free
 * the table slot itself. Kept as a pair regardless, in case a viewer ever
 * exits some other way (a crash, or a future teardown path exo_exit()
 * doesn't cover) that leaves real resources behind after all. */
static page_owner_t last_viewer_id = PAGE_OWNER_FREE;

/* #21 -- build and switch to the WAD/flat/automap viewer as a second, real
 * LibOS context, invoked from the shell's `wadview` command
 * (src/shell/shell_main.c). Takes no arguments: the kernel already knows
 * where the WAD module and the viewer's blobs are.
 *
 * Returns a negative EXO_E* on any failure before the switch is armed, in
 * which case the caller (the shell) keeps running and can report the
 * error. On success, context_switch_request() has armed
 * context_switch_pending and this returns 0 -- but the caller never
 * observes that 0 as an ordinary syscall return: src/syscall_entry.s's
 * epilogue sees the pending switch immediately after this handler returns
 * and takes the context_switch_tail exit into the viewer instead of
 * sysret-ing back to the shell. */
static int64_t sys_launch_wad_viewer(uint64_t a1, uint64_t a2, uint64_t a3,
                                     uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    /* Reclaim the previous viewer's context (pages + framebuffer binding,
     * whichever it still holds) before creating a new one -- see
     * last_viewer_id's own comment. context_lookup() guards against a
     * stale id from a viewer some other path already tore down. */
    if (last_viewer_id != PAGE_OWNER_FREE &&
       context_lookup(last_viewer_id) != NULL) {
        revoke_all(last_viewer_id);
        context_destroy(last_viewer_id);
    }
    last_viewer_id = PAGE_OWNER_FREE;

    uint64_t wad_start, wad_end;
    if (mmap_find_module(&wad_start, &wad_end) != 0) {
        return -EXO_ENODEV;
    }
    uint64_t wad_size = wad_end - wad_start;

    page_owner_t viewer_id;
    int create_rc = context_create(vmm_kernel_pml4(), &viewer_id);
    if (create_rc != CONTEXT_OK) {
        return create_rc == CONTEXT_ENOMEM ? -EXO_ENOMEM : -EXO_EINVAL;
    }

    size_t code_len = (size_t)(_binary_libos_wad_viewer_code_bin_end -
                               _binary_libos_wad_viewer_code_bin_start);
    size_t data_len = (size_t)(_binary_libos_wad_viewer_data_bin_end -
                               _binary_libos_wad_viewer_data_bin_start);

    libos_image_t img;
    if (libos_build_image(viewer_id,
                          _binary_libos_wad_viewer_code_bin_start, code_len,
                          _binary_libos_wad_viewer_data_bin_start, data_len,
                          LIBOS_WAD_VIEWER_BSS_LEN, &img) != VMM_OK) {
        context_destroy(viewer_id);
        return -EXO_ENOMEM;
    }

    if (libos_map_wad((uint64_t *)(uintptr_t)img.pml4_phys,
                      wad_start, wad_size) != VMM_OK) {
        libos_destroy_image(viewer_id, &img);
        context_destroy(viewer_id);
        return -EXO_EINVAL;
    }

    /* SCRUM-175: patch libos_wad_params_t -- the only thing the viewer
     * cannot learn through an ordinary syscall (see src/libos_wad_params.h)
     * -- directly into the viewer's own g_wad_params global (its first
     * .data global) rather than hand-mapping a separate side-channel page
     * the way an earlier version of this mechanism did. See
     * libos_launch_patch_params()'s own comment (src/libos_launch.h) for why
     * this is safe: it requires only that libos_wad_viewer.c stays first in
     * build.sh's source list for this target, which it already is for
     * .text.entry placement. Unlike the old side-channel page (mapped
     * read-only), g_wad_params now lives in the viewer's ordinary writable
     * .data, so the viewer itself could in principle corrupt its own
     * wad_vaddr/wad_size after launch -- an accepted widening, not a
     * regression the kernel needs to guard against, per that same comment. */
    libos_wad_params_t params = {
        .wad_vaddr = LIBOS_WAD_VADDR,
        .wad_size  = wad_size,
    };
    if (libos_launch_patch_params(&img, &params, sizeof(params)) != VMM_OK) {
        libos_destroy_image(viewer_id, &img);
        context_destroy(viewer_id);
        return -EXO_EINVAL;
    }

    /* _irq: this is the viewer's very first entry, reached through
     * context_switch_tail rather than a direct libos_enter_irq() call --
     * without this it would launch with RFLAGS.IF clear (context_prime()'s
     * default, correct for SCRUM-108/109's cooperative-only ping-pong
     * probes) and its exo_get_ticks()/exo_kbd_poll()-driven loops would
     * never see a PIT or keyboard IRQ land. */
    context_prime_irq(viewer_id, img.entry_vaddr, img.stack_top_vaddr);

    /* Whoever currently holds the framebuffer binding -- the shell, from
     * its own libos_fb_map() call at startup (src/shell/shell_main.c), on
     * the very first `wadview`; nobody, on a later one, since the reclaim
     * at the top of this function already released it from the previous
     * viewer -- must give it up before this one can exo_fb_acquire() it
     * (exclusive, src/fb_binding.c, SCRUM-154). fb_binding_owner() rather
     * than assuming the caller: the caller is always the shell, but the
     * *holder* is not, once a second `wadview` runs. Only the binding is
     * released, not the holder's own already-established page-table
     * mapping (fb_binding_release() never touches page tables), so the
     * shell keeps rendering to its existing framebuffer pointer
     * uninterrupted once it is switched back to. Done last, right before
     * the point of no return: every earlier failure path above leaves
     * whoever holds the binding untouched. */
    fb_binding_release(fb_binding_owner());

    /* Succeeds because the caller (the shell) has a real context_t row of
     * its own -- see src/kernel.c's SCRUM-178 refactor of the shell launch.
     * Arms context_switch_pending; src/syscall_entry.s does the actual
     * switch once this handler returns. */
    if (context_switch_request(viewer_id) != CONTEXT_OK) {
        libos_destroy_image(viewer_id, &img);
        context_destroy(viewer_id);
        return -EXO_EINVAL;
    }

    /* Only now, on the success path -- everything above that fails instead
     * destroys viewer_id itself and returns with last_viewer_id still
     * PAGE_OWNER_FREE from the top of this function. */
    last_viewer_id = viewer_id;

    return 0;
}

void syscall_launch_init(void)
{
    exo_syscall_register(EXO_SYS_LAUNCH_WAD_VIEWER, sys_launch_wad_viewer);
}
