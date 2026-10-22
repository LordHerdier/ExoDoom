#include "syscall_rtc.h"
#include "syscall.h"
#include "exo_syscall.h"
#include "errno.h"
#include "rtc.h"

#include <stdint.h>

/* #35 — wall-clock time.
 *   0             *out filled
 *   -EXO_EFAULT   [out, out + sizeof(exo_rtc_time_t)) is not entirely inside
 *                 the LibOS window and mapped writable
 *   -EXO_ENODEV   no CMOS RTC on this machine (per the ACPI FADT)
 *   -EXO_EIO      the RTC did not settle or returned something that is not
 *                 a date
 * No ownership check, like #5: time is not a resource anyone acquires. The
 * pointer is validated the way sys_memstat does (src/syscall_stat.c), before
 * the RTC is touched, and *out is written only on success. */
static int64_t sys_rtc_read(uint64_t out, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (!exo_range_in_user_window(out, sizeof(exo_rtc_time_t)) ||
        !exo_user_range_mapped(out, sizeof(exo_rtc_time_t), 1))
        return -EXO_EFAULT;

    struct rtc_time t;
    int rc = rtc_read(&t);
    if (rc == -ENODEV)
        return -EXO_ENODEV;
    if (rc != 0)
        return -EXO_EIO;

    exo_rtc_time_t *dst = (exo_rtc_time_t *)(uintptr_t)out;
    dst->year      = t.year;
    dst->month     = t.month;
    dst->day       = t.day;
    dst->hour      = t.hour;
    dst->minute    = t.minute;
    dst->second    = t.second;
    dst->reserved  = 0;
    dst->unix_secs = rtc_to_unix(&t);
    return 0;
}

void syscall_rtc_init(void)
{
    exo_syscall_register(EXO_SYS_RTC_READ, sys_rtc_read);
}
