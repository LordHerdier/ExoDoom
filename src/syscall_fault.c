#include "syscall_fault.h"
#include "syscall.h"
#include "exo_syscall.h"
#include "fault_record.h"

#include <stdint.h>

/* #41 — the oldest fault record newer than `after_seq`.
 *   0             *out filled
 *   -EXO_EFAULT   [out, out + sizeof(exo_fault_info_t)) is not entirely inside
 *                 the LibOS window and mapped writable
 *   -EXO_ENOENT   no record newer than after_seq
 * No ownership check, like #22/#38: it reports that a program was ended, not
 * what it held. Pointer validated first, *out written only on success. */
static int64_t sys_fault_info(uint64_t after_seq, uint64_t out, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;

    if (!exo_range_in_user_window(out, sizeof(exo_fault_info_t)) ||
        !exo_user_range_mapped(out, sizeof(exo_fault_info_t), 1))
        return -EXO_EFAULT;

    exo_fault_info_t rec;
    if (fault_record_next(after_seq, &rec) != 0)
        return -EXO_ENOENT;

    *(exo_fault_info_t *)(uintptr_t)out = rec;
    return 0;
}

void syscall_fault_init(void)
{
    exo_syscall_register(EXO_SYS_FAULT_INFO, sys_fault_info);
}
