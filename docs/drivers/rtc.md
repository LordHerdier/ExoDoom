# Driver: CMOS real-time clock

**Files:** `src/rtc.c`, `src/rtc.h`, `src/syscall_rtc.c`, `src/syscall_rtc.h`
**Status:** ✅ Complete (SCRUM-249) **Last updated:** 22 Oct 2026

---

## 1. Purpose

The kernel's only other clock is the PIT's milliseconds-since-boot
(`docs/drivers/pit.md`). This driver reads the battery-backed CMOS RTC and gives
LibOS programs the calendar date and time through `exo_rtc_read` (#35). It is
**polled and read-only**: no IRQ8 (left masked, `pic.c` is untouched), no
alarms, no setting the clock. The RTC is treated as **UTC**; there are no time
zones.

## 2. Hardware

Index port `0x70`, data port `0x71`, through the helpers in `src/io.h`. Every
access writes the index with **bit 7 set** (NMI disable) so an NMI cannot land
between the index write and the data access.

| Register | Meaning |
|---|---|
| `0x00` / `0x02` / `0x04` | seconds / minutes / hours |
| `0x07` / `0x08` / `0x09` | day / month / year (0–99) |
| `0x0A` | Status A; bit 7 = update in progress (UIP) |
| `0x0B` | Status B; bit 1 set = 24-hour, bit 2 set = binary (clear = BCD) |
| `0x0D` | Status D; bit 7 = RTC RAM/battery valid |
| FADT-defined | century (conventionally `0x32`) |

**NMI.** The index port is write-only, so the previous NMI mask cannot be read
back. `rtc_read()` ends by selecting register `0x0D` with bit 7 *clear*, which
leaves NMI unmasked — the state firmware hands over — rather than leaving it
disabled by the last access.

## 3. Reading the clock safely

`rtc_read()`:

1. `-ENODEV` if ACPI says there is no CMOS RTC (`acpi_cmos_rtc_absent()`).
2. `-EIO` if Status D bit 7 is clear (dead battery / invalid RAM).
3. Wait for UIP to clear (bounded spin), read all fields, then wait and read
   again, repeating until **two consecutive reads agree**. This catches a
   rollover that begins mid-read, which would otherwise produce a torn time such
   as `12:59:59` read as `13:59:59`. Bounded retries; `-EIO` if it never settles.
4. Read Status B and honour it: BCD → binary when bit 2 is clear; 12-hour mode
   when bit 1 is clear, where the PM flag is **bit 7 of the hours byte**.
5. Validate every field (`rtc_bcd_valid`, month 1–12, day 1–31, hour 0–23,
   minute/second 0–59) and return `-EIO` for garbage rather than passing it on.

12-hour conversion (`rtc_decode_hour`): `12 AM → 0`, `12 PM → 12`, `1 PM → 13`,
`11 PM → 23`. A value of 0 or above 12 in 12-hour mode is invalid.

## 4. Century

`rtc_init()` caches `acpi_rtc_century_reg()` (the FADT's `Century` byte,
SCRUM-250, `docs/drivers/acpi.md` §5). If it is non-zero the value read from it
is used when it decodes to 19–29. Otherwise — no FADT, no century register, or
a bad value — **20xx is assumed**. `0x32` is deliberately *not* probed blindly:
on real hardware without a century register it can hold unrelated data.

## 5. `rtc_to_unix`

Pure, integer-only (no `double` in `src/`): Howard Hinnant's `days_from_civil`
in signed 64-bit, so it is exact across the 2038 boundary and handles the
Gregorian century rule (2100 is not a leap year; 2000 is). No leap seconds.

## 6. Syscall #35

`exo_rtc_read(exo_rtc_time_t *out)` — `docs/syscall_spec.md` §3.2 row 35.
`year, month, day, hour, minute, second` plus `uint64_t unix_secs`, 16 bytes.
The pointer is validated exactly like `exo_memstat` (`src/syscall_stat.c`):
`exo_range_in_user_window()` then `exo_user_range_mapped(…, writable)`, else
`-EXO_EFAULT` — so NULL, a kernel pointer, an unmapped in-window pointer and a
struct straddling the end of a mapping all return an error instead of
faulting. `*out` is written only on success. No ownership check: time is not a
resource anyone acquires, the same as `exo_get_ticks` (#5).

## 7. Init order

`rtc_init()` and `syscall_rtc_init()` sit beside `syscall_pit_init()` in
`kernel_main`, **ahead of the `TESTING` branch**, after `acpi_init()` (which
must precede `vmm_init()`; it copies the FADT into statics, so the century
register is available here).

## 8. Testing

`tests/kernel/test_rtc_k.c` (suite `rtc`):

- pure: BCD, 24-hour and 12-hour hours including 12 AM / 12 PM, `rtc_to_unix`
  on the epoch, 2024-02-29, 1999/2000, 2100-02-28→03-01 and 2^31 (reference
  values from Python's `calendar.timegm`, not from the code under test);
- live: fields in range, and two reads 1.1 s apart are monotonic and 1–5 s
  apart (compared as Unix times, so a minute or day rollover cannot fail it);
- dispatcher: NULL, kernel pointer, unmapped, straddling pointer, and a valid
  write checked against `rtc_read()`;
- a real ring-3 probe (`tests/kernel/rtc_probe.s`): three `syscall`s from CPL 3
  (valid buffer, NULL, `0x200000`), result bitmask must be 7. The kernel
  address is reachable in a `RING3_PROBE` build, so only the handler's window
  check stops it.

## 9. Gotchas

- Interrupts are off ambiently during tests; a test that waits on the PIT must
  `sti` around the wait (`docs/testing.md`) or `hlt` never returns.
- The RTC has 1-second resolution, so the monotonic live test cannot be shorter
  than a second.
- Out of scope: setting the clock, alarms, IRQ8, time zones, backing libc
  `time()`, drift correction, anchoring `kernel_get_ticks_ms()` to wall time.
