#pragma once

/*
 * rtc.h — CMOS real-time clock driver (SCRUM-249).
 *
 * Polled and read-only: no IRQ8, no alarms, no writes to the clock. The RTC
 * is treated as UTC. The century register comes from the ACPI FADT
 * (acpi_rtc_century_reg(), SCRUM-250); with none, or an implausible value,
 * the century is assumed to be 20.
 *
 * Details: docs/drivers/rtc.md.
 */

#include <stdint.h>

struct rtc_time {
    uint16_t year;      /* full year, e.g. 2026 */
    uint8_t  month;     /* 1..12 */
    uint8_t  day;       /* 1..31 */
    uint8_t  hour;      /* 0..23 */
    uint8_t  minute;    /* 0..59 */
    uint8_t  second;    /* 0..59 */
};

/* ── Pure functions (no hardware access) ────────────────────────────────── */

/* Is every nibble of `v` a decimal digit? */
int rtc_bcd_valid(uint8_t v);

/* Decodes packed BCD. Only meaningful when rtc_bcd_valid(v). */
uint8_t rtc_bcd_to_bin(uint8_t v);

/* Decodes the raw CMOS hours byte to 0..23. `bcd` and `h12` are Status B's
 * "BCD" (bit 2 clear) and "12-hour" (bit 1 clear) meanings. In 12-hour mode
 * bit 7 is the PM flag and the rest is 1..12: 12 AM -> 0, 12 PM -> 12.
 * Returns 0xFF for a value that is not a valid hour. */
uint8_t rtc_decode_hour(uint8_t raw, int bcd, int h12);

/* Seconds since 1970-01-01 00:00:00, integer math only. Fields are assumed
 * valid; years before 1970 return 0. */
uint64_t rtc_to_unix(const struct rtc_time *t);

/* ── Hardware ───────────────────────────────────────────────────────────── */

/* Caches the century register and logs the clock to serial. Safe to call
 * once ACPI is up; does not enable any interrupt. */
void rtc_init(void);

/* Fills *out from the CMOS. 0 on success; -ENODEV when ACPI says the machine
 * has no CMOS RTC; -EIO when the clock never settles, reports invalid RAM, or
 * returns values that are not a date. */
int rtc_read(struct rtc_time *out);
