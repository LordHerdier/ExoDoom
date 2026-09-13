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
