#include "libos_test_common.h"
#include "libos_launch.h"
#include "fault.h"
#include "syscall.h"
#include "vmm.h"
#include "page_alloc.h"

void libos_test_teardown_owner(page_owner_t owner)
{
    fault_set_hook(0);
    exo_syscall_register(LIBOS_RETURN_SYSCALL_NUM, 0);

    if (vmm_address_space_for(owner) != 0) {
        vmm_switch_address_space(vmm_kernel_pml4());
        page_reclaim_all(owner);
        vmm_destroy_address_space(owner);
    }
}

/* Not expected to fire in the happy path -- installed for every
 * libos_test_launch() call anyway so a regression (launched code touching
 * something unmapped) reports as a failed assertion on fault_count instead
 * of a triple fault taking the whole suite down with it. */
static volatile int libos_test_fault_count;

static int libos_test_recording_hook(exception_frame_t *f, uint64_t cr2)
{
    (void)f; (void)cr2;
    libos_test_fault_count++;
    return 0;
}

libos_test_launch_result_t libos_test_launch(const libos_image_t *img)
{
    libos_test_launch_result_t r = { 0, 0, 0, 0 };

    libos_test_fault_count = 0;
    fault_set_hook(libos_test_recording_hook);
    exo_syscall_register(LIBOS_RETURN_SYSCALL_NUM, libos_return);

    r.switch_in_status  = vmm_switch_address_space(img->pml4_phys);
    if (r.switch_in_status == VMM_OK) {
        r.result            = libos_enter(img->entry_vaddr, img->stack_top_vaddr);
        r.switch_out_status = vmm_switch_address_space(vmm_kernel_pml4());
    }

    exo_syscall_register(LIBOS_RETURN_SYSCALL_NUM, 0);
    fault_set_hook(0);

    r.fault_count = libos_test_fault_count;
    return r;
}
