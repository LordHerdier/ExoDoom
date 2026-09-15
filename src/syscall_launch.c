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

    /* One owned page carrying libos_wad_params_t -- the only thing the
     * viewer cannot learn through an ordinary syscall (see
     * src/libos_wad_params.h). Mapped read-only: the viewer never writes
     * it. */
    void *params_page = alloc_page_owned(viewer_id);
    if (params_page == NULL) {
        libos_destroy_image(viewer_id, &img);
        context_destroy(viewer_id);
        return -EXO_ENOMEM;
    }
    *(libos_wad_params_t *)params_page = (libos_wad_params_t){
        .wad_vaddr = LIBOS_WAD_VADDR,
        .wad_size  = wad_size,
    };
    if (vmm_map_page_in((uint64_t *)(uintptr_t)img.pml4_phys,
                        LIBOS_WAD_PARAMS_VADDR,
                        (uint64_t)(uintptr_t)params_page,
                        VMM_PRESENT | VMM_USER) != VMM_OK) {
        free_page_owned(params_page, viewer_id);
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

    /* The shell already holds the framebuffer binding from its own
     * libos_fb_map() call at startup (src/shell/shell_main.c) -- exo_fb_
     * acquire is exclusive (src/fb_binding.c, SCRUM-154), so the viewer's
     * own libos_fb_map() would get -EXO_EBUSY without this. Only the
     * *binding* is released, not the shell's own already-established
     * page-table mapping (fb_binding_release() never touches page tables),
     * so the shell keeps rendering to its existing framebuffer pointer
     * uninterrupted once it is switched back to -- it never calls
     * libos_fb_map() a second time, so it never notices the binding moved.
     * Done last, right before the point of no return: every earlier
     * failure path above keeps the shell's binding intact. */
    fb_binding_release(context_current());

    /* Succeeds because the caller (the shell) has a real context_t row of
     * its own -- see src/kernel.c's SCRUM-178 refactor of the shell launch.
     * Arms context_switch_pending; src/syscall_entry.s does the actual
     * switch once this handler returns. */
    if (context_switch_request(viewer_id) != CONTEXT_OK) {
        free_page_owned(params_page, viewer_id);
        libos_destroy_image(viewer_id, &img);
        context_destroy(viewer_id);
        return -EXO_EINVAL;
    }

    return 0;
}

void syscall_launch_init(void)
{
    exo_syscall_register(EXO_SYS_LAUNCH_WAD_VIEWER, sys_launch_wad_viewer);
}
