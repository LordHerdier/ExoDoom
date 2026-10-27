#include "shell_launch.h"

#include <stddef.h>
#include <stdint.h>

#include "context.h"
#include "libos_launch.h"
#include "vmm.h"
#include "shell/shell_layout.h"

/* The embedded shell blobs; see the comment on the same externs in kernel.c. */
extern const uint8_t _binary_shell_code_bin_start[];
extern const uint8_t _binary_shell_code_bin_end[];
extern const uint8_t _binary_shell_data_bin_start[];
extern const uint8_t _binary_shell_data_bin_end[];

int shell_launch_context(page_owner_t *id_out)
{
    page_owner_t id;

    /* vmm_kernel_pml4() is the placeholder binding kernel_main uses too:
     * libos_build_image() rebinds this id to the shell's own address space. */
    if (context_create(vmm_kernel_pml4(), &id) != CONTEXT_OK)
        return -1;
    context_set_name(id, "shell");

    /* static: libos_image_t is a few KiB, and this runs on the 16 KiB
     * exception stack.  Not reentrant, which is fine: a second fault while
     * this runs hits the in_fault guard and halts. */
    static libos_image_t img;
    if (libos_build_image(id, _binary_shell_code_bin_start,
                          (size_t)(_binary_shell_code_bin_end -
                                   _binary_shell_code_bin_start),
                          _binary_shell_data_bin_start,
                          (size_t)(_binary_shell_data_bin_end -
                                   _binary_shell_data_bin_start),
                          SHELL_BSS_LEN, &img) != VMM_OK) {
        /* Same cleanup src/syscall_launch.c does for a failed build. */
        context_destroy(id);
        return -1;
    }

    if (context_prime_irq(id, img.entry_vaddr, img.stack_top_vaddr) != CONTEXT_OK) {
        libos_destroy_image(id, &img);
        context_destroy(id);
        return -1;
    }

    *id_out = id;
    return 0;
}
