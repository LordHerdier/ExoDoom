/*
 * rtc.c — CMOS real-time clock driver (SCRUM-249). See rtc.h and
 * docs/drivers/rtc.md.
 */

#include "rtc.h"
#include "acpi.h"
#include "errno.h"
#include "io.h"
#include "serial.h"

#define CMOS_INDEX   0x70
#define CMOS_DATA    0x71
#define CMOS_NMI_OFF 0x80   /* bit 7 of the index byte */

#define CMOS_SECONDS 0x00
#define CMOS_MINUTES 0x02
#define CMOS_HOURS   0x04
#define CMOS_DAY     0x07
#define CMOS_MONTH   0x08
#define CMOS_YEAR    0x09
#define CMOS_STATUS_A 0x0A
#define CMOS_STATUS_B 0x0B
#define CMOS_STATUS_D 0x0D

#define STATUS_A_UIP     0x80   /* update in progress */
#define STATUS_B_24H     0x02   /* set: 24-hour hours byte */
#define STATUS_B_BINARY  0x04   /* set: binary, clear: BCD */
#define STATUS_D_VRT     0x80   /* RTC RAM/battery valid */

/* The UIP flag is up for under two milliseconds once a second. Every inb is
 * a port exit under virtualization (microseconds), so this bound is generous
 * on bare metal and still finite in the worst case. */
#define UIP_SPIN_MAX     2000000u
#define READ_RETRIES     8

#define CENTURY_MIN      19
#define CENTURY_MAX      29
#define CENTURY_DEFAULT  20

/* ── Pure functions ─────────────────────────────────────────────────────── */

int rtc_bcd_valid(uint8_t v)
{
    return (v & 0x0F) <= 9 && (v >> 4) <= 9;
}

uint8_t rtc_bcd_to_bin(uint8_t v)
{
    return (uint8_t)((v >> 4) * 10 + (v & 0x0F));
}

uint8_t rtc_decode_hour(uint8_t raw, int bcd, int h12)
{
    int pm = 0;
    uint8_t h = raw;

    if (h12) {
        pm = (raw & 0x80) != 0;
        h = raw & 0x7F;
    }
    if (bcd) {
        if (!rtc_bcd_valid(h))
            return 0xFF;
        h = rtc_bcd_to_bin(h);
    }
    if (h12) {
        if (h < 1 || h > 12)
            return 0xFF;
        h = (uint8_t)((h % 12) + (pm ? 12 : 0));
    }
    return h < 24 ? h : 0xFF;
}

/* Days from 1970-01-01 to a proleptic Gregorian date (Howard Hinnant's
 * days_from_civil), kept in signed 64-bit so the era arithmetic is plain. */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

uint64_t rtc_to_unix(const struct rtc_time *t)
{
    int64_t days = days_from_civil(t->year, t->month, t->day);
    int64_t secs = days * 86400 + (int64_t)t->hour * 3600 +
                   (int64_t)t->minute * 60 + t->second;
    return secs < 0 ? 0 : (uint64_t)secs;
}

/* ── Hardware ───────────────────────────────────────────────────────────── */

static uint8_t century_reg;   /* 0 = none, assume 20xx */

/* Every access sets the NMI-disable bit so an NMI cannot land between the
 * index write and the data access. rtc_read() puts the index back with the
 * bit clear before returning, so NMIs are not left masked. */
static uint8_t cmos_read(uint8_t reg)
{
    outb(CMOS_INDEX, (uint8_t)(CMOS_NMI_OFF | reg));
    return inb(CMOS_DATA);
}

struct raw_time {
    uint8_t sec, min, hour, day, month, year, century;
};

static void read_raw(struct raw_time *r)
{
    r->sec   = cmos_read(CMOS_SECONDS);
    r->min   = cmos_read(CMOS_MINUTES);
    r->hour  = cmos_read(CMOS_HOURS);
    r->day   = cmos_read(CMOS_DAY);
    r->month = cmos_read(CMOS_MONTH);
    r->year  = cmos_read(CMOS_YEAR);
    r->century = century_reg ? cmos_read(century_reg) : 0;
}

static int raw_equal(const struct raw_time *a, const struct raw_time *b)
{
    return a->sec == b->sec && a->min == b->min && a->hour == b->hour &&
           a->day == b->day && a->month == b->month && a->year == b->year &&
           a->century == b->century;
}

static int wait_not_updating(void)
{
    for (uint32_t i = 0; i < UIP_SPIN_MAX; i++) {
        if (!(cmos_read(CMOS_STATUS_A) & STATUS_A_UIP))
            return 0;
    }
    return -1;
}

/* One 0..99-style field: decoded per Status B, or -1 if it is not valid BCD. */
static int field(uint8_t v, int bcd)
{
    if (!bcd)
        return v;
    return rtc_bcd_valid(v) ? rtc_bcd_to_bin(v) : -1;
}

static int decode(const struct raw_time *r, uint8_t status_b,
                  struct rtc_time *out)
{
    int bcd = !(status_b & STATUS_B_BINARY);
    int h12 = !(status_b & STATUS_B_24H);

    int sec = field(r->sec, bcd), min = field(r->min, bcd);
    int day = field(r->day, bcd), month = field(r->month, bcd);
    int yy = field(r->year, bcd);
    uint8_t hour = rtc_decode_hour(r->hour, bcd, h12);

    int century = CENTURY_DEFAULT;
    if (century_reg) {
        int c = field(r->century, bcd);
        if (c >= CENTURY_MIN && c <= CENTURY_MAX)
            century = c;
    }

    if (sec < 0 || sec > 59 || min < 0 || min > 59 || hour == 0xFF ||
        day < 1 || day > 31 || month < 1 || month > 12 || yy < 0 || yy > 99)
        return -EIO;

    out->year   = (uint16_t)(century * 100 + yy);
    out->month  = (uint8_t)month;
    out->day    = (uint8_t)day;
    out->hour   = hour;
    out->minute = (uint8_t)min;
    out->second = (uint8_t)sec;
    return 0;
}

int rtc_read(struct rtc_time *out)
{
    int rc = -EIO;

    if (acpi_cmos_rtc_absent())
        return -ENODEV;

    if (!(cmos_read(CMOS_STATUS_D) & STATUS_D_VRT))
        goto done;

    /* Wait out any update, read, and repeat until two consecutive reads
     * agree: an update that begins mid-read shows up as a mismatch. */
    struct raw_time a, b;
    int settled = 0;
    if (wait_not_updating() == 0) {
        read_raw(&a);
        for (int i = 0; i < READ_RETRIES; i++) {
            if (wait_not_updating() != 0)
                break;
            read_raw(&b);
            if (raw_equal(&a, &b)) {
                settled = 1;
                break;
            }
            a = b;
        }
    }
    if (!settled)
        goto done;

    /* Status B changes only if firmware rewrites it; once is enough. */
    rc = decode(&a, cmos_read(CMOS_STATUS_B), out);

done:
    /* Re-select a register with bit 7 clear: leaves NMI unmasked. */
    outb(CMOS_INDEX, CMOS_STATUS_D);
    return rc;
}

void rtc_init(void)
{
    century_reg = acpi_rtc_century_reg();

    serial_print("RTC: century register ");
    if (century_reg) {
        serial_print("0x");
        serial_print_hex(century_reg);
    } else {
        serial_print("none (assuming 20xx)");
    }
    serial_print("\n");

    struct rtc_time t;
    int rc = rtc_read(&t);
    if (rc == 0) {
        serial_print("RTC: ");
        serial_print_dec(t.year);
        serial_print("-");
        serial_print_dec(t.month);
        serial_print("-");
        serial_print_dec(t.day);
        serial_print(" ");
        serial_print_dec(t.hour);
        serial_print(":");
        serial_print_dec(t.minute);
        serial_print(":");
        serial_print_dec(t.second);
        serial_print(" UTC\n");
    } else {
        serial_print("RTC: unavailable (rc=");
        serial_print_hex64((uint64_t)(int64_t)rc);
        serial_print(")\n");
    }
}
