# Driver: ACPI table parser

**Files:** `src/acpi.c`, `src/acpi.h` **Status:** ✅ Complete (SCRUM-250)
**Last updated:** 22 Oct 2026

---

## 1. Purpose

A read-only parser for the firmware's ACPI tables. It exists for SCRUM-249's RTC
driver, which needs the CMOS **century register** (`FADT.Century`) and whether a
CMOS RTC exists at all (`FADT.IAPC_BOOT_ARCH` bit 5). It is **not** an AML
interpreter: no namespace, no `_S5` power-off, no MADT/APIC use. The public API
is deliberately narrow so uACPI can replace the internals later (it would serve
`acpi_fadt()` via `uacpi_table_fadt()`; the pure functions and their tests
become the early-boot path).

## 2. API

| Function | Meaning |
|---|---|
| `acpi_init(mb)` | 0 on success, `-errno` otherwise; never halts; idempotent |
| `acpi_table_listed(sig)` | was this table in the root table's directory? |
| `acpi_fadt()` | the kernel's private FADT copy, or NULL |
| `acpi_rtc_century_reg()` | CMOS register index, 0 = none/unknown |
| `acpi_cmos_rtc_absent()` | `IAPC_BOOT_ARCH` bit 5 |

The parsing core (`acpi_checksum_ok`, `acpi_rsdp_validate`, `acpi_root_entries`,
`acpi_root_find`, `acpi_fadt_extract`, `acpi_century_reg_plausible`) is pure:
buffers in, results out, so `tests/kernel/test_acpi_k.c` feeds it hand-built
tables.

## 3. Discovery and parsing

1. **RSDP.** Multiboot2 tag 15 (ACPI 2.0+ copy), then tag 14 (1.0 copy); GRUB
   embeds a verbatim RSDP in each and needs no request. Otherwise scan
   `0xE0000–0xFFFFF` on 16-byte boundaries for `"RSD PTR "`. The EBDA is **not**
   scanned: its segment pointer is at `0x40E`, in page 0, the unmapped NULL guard.
2. **Validate** signature, the 20-byte checksum and, for revision ≥ 2, the
   extended checksum over `length`.
3. **Root table.** XSDT when revision ≥ 2 and it validates and holds a usable
   FADT; otherwise the RSDT. A broken XSDT falls back rather than failing.
   Entry count is `(length − 36) / 8` (XSDT) or `/ 4` (RSDT); length is bounded
   to `[36, 1 MiB]`. XSDT entries are read with `memcpy` (4-byte aligned only).
   An entry is skipped unless its whole header fits below 4 GiB, and a table is
   rejected unless its whole `length` does. A skipped table is logged on serial.
   State is built in locals and committed only on success.
4. **FADT** (`"FACP"`): Century at offset 108, `IAPC_BOOT_ARCH` (16-bit) at 109.
   `header.length` is checked before each read (Century needs ≥ 109, IAPC ≥ 111),
   and IAPC is only read from FADT revision ≥ 3 (reserved before that); an
   absent field reads 0. A Century outside `[0x0E, 0x80)` is reported as 0.

## 4. Ordering: before `vmm_init`

`vmm_init` replaces the boot map's 4 GB identity map with tables that cover only
low memory, the kernel image, usable RAM and the framebuffer. Firmware tables
live in ACPI-reclaimable/reserved memory, which is **not** mapped afterwards, so
reading one after `vmm_init` page-faults. `acpi_init()` runs after `idt_init()`/`tss_init()` (so a fault while walking
firmware pointers gets a serial diagnostic) and before `page_alloc_init` — it
needs only serial — and copies the FADT (≤ 320 bytes) and a
directory of up to 32 table signatures into statics. No function here touches
firmware memory after `acpi_init` returns. It sits ahead of the `TESTING`
branch so the live tests can read the cached state. `page_alloc` hands out only
`MULTIBOOT_MMAP_AVAILABLE` regions, so reclaimable memory is never allocated.

Failure is non-fatal: `kernel_main` warns on serial and the accessors report
"unknown".

## 5. How the RTC consumes it (SCRUM-249)

- `acpi_rtc_century_reg()` non-zero → use that register, sanity-checking the BCD
  value read from it (fall back to 20 if outside 19–29).
- ACPI failed, or no century register → assume 20xx. `0x32` is **not** probed
  blindly; on real hardware it can hold unrelated data.
- `acpi_cmos_rtc_absent()` → `rtc_read()` returns `-ENODEV`.

## 6. Testing

`suite_acpi_tests`: checksum, RSDP v1/v2 and every rejection (signature,
checksum, extended checksum, truncation), RSDT/XSDT walk (>4 GiB entry, wrong
kind, oversize/undersize length, corrupt table), FADT extraction at full and
truncated lengths, century plausibility, and live checks against QEMU's real
tables (FADT copy valid, `APIC` listed, century register plausible).

## 7. Gotchas

- Keep `acpi_init()` before `vmm_init()`. Moving it later turns a firmware read
  into a page fault.
- Direct kernel boot may not supply the ACPI tags; the BIOS-area scan is the
  fallback.
- No `double`, no SSE; signature compares use `memcmp`; structs are packed.
