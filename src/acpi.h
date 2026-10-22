#pragma once

/*
 * ACPI table parser (SCRUM-250): RSDP -> RSDT/XSDT -> FADT, read-only.
 *
 * Deliberately not an AML interpreter. It exists to answer what the RTC
 * driver (SCRUM-249) needs -- the CMOS century register and whether a CMOS
 * RTC exists -- and keeps its public surface narrow so uACPI can replace the
 * internals later without touching callers.
 *
 * acpi_init() MUST run before vmm_init(): the kernel map does not map
 * ACPI-reclaimable or reserved memory, so firmware tables are only readable
 * on the boot map. Everything later callers need is copied into statics.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "multiboot2.h"

#define ACPI_SIG_LEN         4
#define ACPI_MAX_TABLE_LEN   (1u << 20)   /* reject anything larger          */
#define ACPI_FADT_COPY_MAX   320          /* ACPI 6.x FADT is 276 bytes      */
#define ACPI_DIR_MAX         32           /* table signatures we remember    */

struct acpi_sdt_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

struct acpi_rsdp {
    char     signature[8];        /* "RSD PTR "                              */
    uint8_t  checksum;            /* covers the first 20 bytes               */
    char     oem_id[6];
    uint8_t  revision;            /* 0 = ACPI 1.0, 2 = ACPI 2.0+             */
    uint32_t rsdt_addr;
    uint32_t length;              /* revision >= 2 only                      */
    uint64_t xsdt_addr;           /* revision >= 2 only                      */
    uint8_t  ext_checksum;        /* covers `length` bytes                   */
    uint8_t  reserved[3];
} __attribute__((packed));

#define ACPI_RSDP_V1_SIZE   20
#define ACPI_FADT_CENTURY_OFF    108   /* 1 byte                             */
#define ACPI_FADT_IAPC_OFF       109   /* 2 bytes (ACPI 2.0+)                */
#define ACPI_IAPC_NO_CMOS_RTC    (1u << 5)

struct acpi_fadt_info {
    uint8_t  century_reg;         /* 0 = absent or unusable                  */
    uint16_t iapc_boot_arch;      /* 0 when the field is not present         */
};

/* ── Pure functions (buffers in, results out; no hardware or globals) ───── */

/* Byte sum of [p, p+len) is 0 mod 256. A zero-length buffer is NOT ok. */
bool acpi_checksum_ok(const void *p, size_t len);

/* `p` must hold at least `avail` bytes. Returns p on a valid RSDP (signature,
 * checksum, and for revision >= 2 the extended checksum), NULL otherwise. */
const struct acpi_rsdp *acpi_rsdp_validate(const void *p, size_t avail);

/* Validates the root table's header (signature XSDT/RSDT per is_xsdt, length
 * bounds, checksum) and writes up to `max` 64-bit table addresses to `out`.
 * Returns the number written, or -EINVAL for a bad root. Entries at or above
 * 4 GiB are skipped (unreachable through the boot identity map). */
int acpi_root_entries(const struct acpi_sdt_header *root, bool is_xsdt,
                      uint64_t *out, int max);

/* First table in the root's directory with signature `sig` whose header and
 * checksum validate; NULL if none. */
const struct acpi_sdt_header *acpi_root_find(const struct acpi_sdt_header *root,
                                             bool is_xsdt, const char sig[4]);

/* Reads Century and IAPC_BOOT_ARCH out of a FADT, honouring header.length.
 * Returns 0, or -EINVAL when `fadt` is not a valid-looking "FACP". */
int acpi_fadt_extract(const struct acpi_sdt_header *fadt,
                      struct acpi_fadt_info *out);

/* Is `v` a usable CMOS century register index? (not 0, not inside the clock
 * registers 0x00-0x0D, not >= 0x80 which would be the NMI-disable bit) */
bool acpi_century_reg_plausible(uint8_t v);

/* ── Kernel state ───────────────────────────────────────────────────────── */

/* 0 on success. Idempotent. On failure logs to serial and returns -errno; the
 * accessors below then report "unknown". */
int acpi_init(const struct mb2_info *mb);

bool acpi_table_listed(const char sig[4]);
const struct acpi_sdt_header *acpi_fadt(void);
uint8_t acpi_rtc_century_reg(void);
bool acpi_cmos_rtc_absent(void);
