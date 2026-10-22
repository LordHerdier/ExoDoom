/*
 * test_acpi_k.c — src/acpi.c (SCRUM-250).
 *
 * Two halves. The pure-function tests build RSDPs, root tables and FADTs by
 * hand in static buffers and feed them to the parser, so every rejection path
 * (bad checksum, wrong signature, oversize length, >4 GiB entry, truncated
 * FADT) is exercised without depending on what the firmware happens to hold.
 * The live tests check the state kernel_main's acpi_init() left behind from
 * QEMU's real tables.
 *
 * Buffers live in the kernel image, which is below 4 GiB, so their addresses
 * fit RSDT entries and are valid on the boot and kernel maps alike.
 */

#include "kunit.h"
#include "acpi.h"
#include "errno.h"
#include "mmap.h"
#include "string.h"

#define FADT_LEN   276
#define APIC_LEN   44

static uint8_t fadt_buf[FADT_LEN]   __attribute__((aligned(8)));
static uint8_t apic_buf[APIC_LEN]   __attribute__((aligned(8)));
static uint8_t rsdt_buf[36 + 4 * 3] __attribute__((aligned(8)));
static uint8_t xsdt_buf[36 + 8 * 3] __attribute__((aligned(8)));
static uint8_t rsdp_buf[36]         __attribute__((aligned(8)));

/* Make the byte at `off` the value that brings the sum of [0,len) to 0. */
static void fix_csum(uint8_t *b, uint32_t len, uint32_t off)
{
    b[off] = 0;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) {
        sum = (uint8_t)(sum + b[i]);
    }
    b[off] = (uint8_t)(0u - sum);
}

static void mk_sdt(uint8_t *b, const char *sig, uint32_t len)
{
    memset(b, 0, len);
    memcpy(b, sig, 4);
    memcpy(b + 4, &len, 4);
    b[8] = 1;
    fix_csum(b, len, 9);
}

static void set_len(uint8_t *b, uint32_t len)
{
    memcpy(b + 4, &len, 4);
    fix_csum(b, len, 9);
}

/* FACP: Century at 108, IAPC_BOOT_ARCH at 109. Rebuilt for every test. */
static void mk_fadt(uint32_t len, uint8_t century, uint16_t iapc)
{
    mk_sdt(fadt_buf, "FACP", len);
    fadt_buf[8] = 5;                    /* IAPC_BOOT_ARCH needs revision >= 3 */
    if (len > ACPI_FADT_CENTURY_OFF) {
        fadt_buf[ACPI_FADT_CENTURY_OFF] = century;
    }
    if (len >= ACPI_FADT_IAPC_OFF + 2u) {
        memcpy(fadt_buf + ACPI_FADT_IAPC_OFF, &iapc, 2);
    }
    fix_csum(fadt_buf, len, 9);
}

static void mk_rsdt(void)
{
    uint32_t e[3] = { 0, 0, 0 };
    mk_fadt(FADT_LEN, 0x32, 0);
    mk_sdt(apic_buf, "APIC", APIC_LEN);
    e[0] = (uint32_t)(uintptr_t)fadt_buf;
    e[1] = (uint32_t)(uintptr_t)apic_buf;
    e[2] = 0;                                   /* a null entry is skipped */
    mk_sdt(rsdt_buf, "RSDT", sizeof(rsdt_buf));
    memcpy(rsdt_buf + 36, e, sizeof(e));
    fix_csum(rsdt_buf, sizeof(rsdt_buf), 9);
}

static void mk_xsdt(uint64_t third)
{
    uint64_t e[3];
    mk_fadt(FADT_LEN, 0x32, 0);
    mk_sdt(apic_buf, "APIC", APIC_LEN);
    e[0] = (uint64_t)(uintptr_t)fadt_buf;
    e[1] = (uint64_t)(uintptr_t)apic_buf;
    e[2] = third;
    mk_sdt(xsdt_buf, "XSDT", sizeof(xsdt_buf));
    memcpy(xsdt_buf + 36, e, sizeof(e));
    fix_csum(xsdt_buf, sizeof(xsdt_buf), 9);
}

static void mk_rsdp(uint8_t rev)
{
    memset(rsdp_buf, 0, sizeof(rsdp_buf));
    memcpy(rsdp_buf, "RSD PTR ", 8);
    rsdp_buf[15] = rev;
    if (rev >= 2) {
        uint32_t len = 36;
        memcpy(rsdp_buf + 20, &len, 4);
    }
    fix_csum(rsdp_buf, 20, 8);
    if (rev >= 2) {
        fix_csum(rsdp_buf, 36, 32);
    }
}

/* ── Checksum ───────────────────────────────────────────────────────────── */

static void test_checksum_good_and_bad(void)
{
    uint8_t b[4] = { 1, 2, 3, 0 };
    fix_csum(b, 4, 3);
    CU_ASSERT_TRUE(acpi_checksum_ok(b, 4));
    b[1]++;
    CU_ASSERT_FALSE(acpi_checksum_ok(b, 4));
}

static void test_checksum_rejects_empty(void)
{
    uint8_t b[1] = { 0 };
    CU_ASSERT_FALSE(acpi_checksum_ok(b, 0));
    CU_ASSERT_FALSE(acpi_checksum_ok(NULL, 4));
}

/* ── RSDP ───────────────────────────────────────────────────────────────── */

static void test_rsdp_v1_valid(void)
{
    mk_rsdp(0);
    CU_ASSERT_TRUE((const void *)(acpi_rsdp_validate(rsdp_buf, 20)) == (const void *)(void *)rsdp_buf);
}

static void test_rsdp_v2_valid(void)
{
    mk_rsdp(2);
    CU_ASSERT_TRUE((const void *)(acpi_rsdp_validate(rsdp_buf, 36)) == (const void *)(void *)rsdp_buf);
}

static void test_rsdp_bad_signature(void)
{
    mk_rsdp(0);
    rsdp_buf[0] = 'X';
    fix_csum(rsdp_buf, 20, 8);
    CU_ASSERT_PTR_NULL(acpi_rsdp_validate(rsdp_buf, 20));
}

static void test_rsdp_bad_checksum(void)
{
    mk_rsdp(0);
    rsdp_buf[9]++;
    CU_ASSERT_PTR_NULL(acpi_rsdp_validate(rsdp_buf, 20));
}

static void test_rsdp_bad_extended_checksum(void)
{
    mk_rsdp(2);
    rsdp_buf[24]++;                 /* inside xsdt_addr: v1 sum still good */
    CU_ASSERT_PTR_NULL(acpi_rsdp_validate(rsdp_buf, 36));
}

static void test_rsdp_truncated(void)
{
    mk_rsdp(2);
    CU_ASSERT_PTR_NULL(acpi_rsdp_validate(rsdp_buf, 19));
    CU_ASSERT_PTR_NULL(acpi_rsdp_validate(rsdp_buf, 35));   /* v2 needs 36 */
}

/* ── Root table walk ────────────────────────────────────────────────────── */

static void test_rsdt_entries_and_find(void)
{
    uint64_t out[8];
    mk_rsdt();
    int n = acpi_root_entries((const struct acpi_sdt_header *)rsdt_buf, false, out, 8);
    CU_ASSERT_EQUAL(n, 2);                      /* null entry skipped */
    CU_ASSERT_TRUE((const void *)(acpi_root_find((const struct acpi_sdt_header *)rsdt_buf, false, "FACP")) == (const void *)(void *)fadt_buf);
    CU_ASSERT_TRUE((const void *)(acpi_root_find((const struct acpi_sdt_header *)rsdt_buf, false, "APIC")) == (const void *)(void *)apic_buf);
    CU_ASSERT_PTR_NULL(
        acpi_root_find((const struct acpi_sdt_header *)rsdt_buf, false, "HPET"));
}

static void test_xsdt_entries_and_find(void)
{
    uint64_t out[8];
    mk_xsdt(0);
    int n = acpi_root_entries((const struct acpi_sdt_header *)xsdt_buf, true, out, 8);
    CU_ASSERT_EQUAL(n, 2);
    CU_ASSERT_EQUAL(out[0], (uint64_t)(uintptr_t)fadt_buf);
    CU_ASSERT_TRUE((const void *)(acpi_root_find((const struct acpi_sdt_header *)xsdt_buf, true, "FACP")) == (const void *)(void *)fadt_buf);
}

static void test_xsdt_entry_above_4gib_skipped(void)
{
    uint64_t out[8];
    mk_xsdt(0x100000000ULL);
    int n = acpi_root_entries((const struct acpi_sdt_header *)xsdt_buf, true, out, 8);
    CU_ASSERT_EQUAL(n, 2);
}

/* The header must fit below 4 GiB, not merely start there. */
static void test_xsdt_entry_straddling_4gib_skipped(void)
{
    uint64_t out[8];
    mk_xsdt(0xFFFFFFF0ULL);
    int n = acpi_root_entries((const struct acpi_sdt_header *)xsdt_buf, true, out, 8);
    CU_ASSERT_EQUAL(n, 2);
}

static void test_root_max_caps_output(void)
{
    uint64_t out[1];
    mk_rsdt();
    CU_ASSERT_EQUAL(
        acpi_root_entries((const struct acpi_sdt_header *)rsdt_buf, false, out, 1), 1);
}

static void test_root_wrong_signature_for_kind(void)
{
    uint64_t out[8];
    mk_rsdt();
    CU_ASSERT_EQUAL(
        acpi_root_entries((const struct acpi_sdt_header *)rsdt_buf, true, out, 8),
        -EINVAL);
}

/* A bad XSDT checksum makes the root unusable -- which is the condition
 * acpi_init() falls back to the RSDT on. */
static void test_xsdt_bad_checksum_rejected_rsdt_still_good(void)
{
    uint64_t out[8];
    mk_rsdt();
    mk_xsdt(0);
    xsdt_buf[40]++;
    CU_ASSERT_EQUAL(
        acpi_root_entries((const struct acpi_sdt_header *)xsdt_buf, true, out, 8),
        -EINVAL);
    CU_ASSERT_PTR_NOT_NULL(
        acpi_root_find((const struct acpi_sdt_header *)rsdt_buf, false, "FACP"));
}

static void test_root_oversize_length_rejected(void)
{
    uint64_t out[8];
    mk_rsdt();
    /* Poke the length field only: set_len() would run fix_csum over ~1 MiB of
     * this 48-byte buffer. The parser's own cap must reject it before it ever
     * sums that many bytes. */
    uint32_t huge = ACPI_MAX_TABLE_LEN + 1;
    memcpy(rsdt_buf + 4, &huge, 4);
    CU_ASSERT_EQUAL(
        acpi_root_entries((const struct acpi_sdt_header *)rsdt_buf, false, out, 8),
        -EINVAL);
}

static void test_root_undersize_length_rejected(void)
{
    uint64_t out[8];
    mk_rsdt();
    set_len(rsdt_buf, 20);
    CU_ASSERT_EQUAL(
        acpi_root_entries((const struct acpi_sdt_header *)rsdt_buf, false, out, 8),
        -EINVAL);
}

static void test_listed_table_with_bad_checksum_not_found(void)
{
    mk_rsdt();
    apic_buf[20]++;
    CU_ASSERT_PTR_NULL(
        acpi_root_find((const struct acpi_sdt_header *)rsdt_buf, false, "APIC"));
    CU_ASSERT_PTR_NOT_NULL(
        acpi_root_find((const struct acpi_sdt_header *)rsdt_buf, false, "FACP"));
}

/* ── FADT extraction ────────────────────────────────────────────────────── */

static void test_fadt_full_length(void)
{
    struct acpi_fadt_info info;
    mk_fadt(FADT_LEN, 0x32, ACPI_IAPC_NO_CMOS_RTC);
    CU_ASSERT_EQUAL(acpi_fadt_extract((const struct acpi_sdt_header *)fadt_buf, &info), 0);
    CU_ASSERT_EQUAL(info.century_reg, 0x32);
    CU_ASSERT_EQUAL(info.iapc_boot_arch, ACPI_IAPC_NO_CMOS_RTC);
}

static void test_fadt_old_revision_ignores_iapc(void)
{
    struct acpi_fadt_info info;
    mk_fadt(FADT_LEN, 0x32, ACPI_IAPC_NO_CMOS_RTC);
    fadt_buf[8] = 1;                     /* revision 1: bytes 109-110 reserved */
    CU_ASSERT_EQUAL(acpi_fadt_extract((const struct acpi_sdt_header *)fadt_buf, &info), 0);
    CU_ASSERT_EQUAL(info.century_reg, 0x32);
    CU_ASSERT_EQUAL(info.iapc_boot_arch, 0);
}

static void test_fadt_truncated_before_iapc(void)
{
    struct acpi_fadt_info info;
    mk_fadt(110, 0x32, 0xFFFF);          /* IAPC needs length >= 111 */
    CU_ASSERT_EQUAL(acpi_fadt_extract((const struct acpi_sdt_header *)fadt_buf, &info), 0);
    CU_ASSERT_EQUAL(info.century_reg, 0x32);
    CU_ASSERT_EQUAL(info.iapc_boot_arch, 0);
}

static void test_fadt_truncated_before_century(void)
{
    struct acpi_fadt_info info;
    mk_fadt(108, 0x32, 0);               /* Century needs length >= 109 */
    CU_ASSERT_EQUAL(acpi_fadt_extract((const struct acpi_sdt_header *)fadt_buf, &info), 0);
    CU_ASSERT_EQUAL(info.century_reg, 0);
    CU_ASSERT_EQUAL(info.iapc_boot_arch, 0);
}

static void test_fadt_wrong_signature(void)
{
    struct acpi_fadt_info info;
    mk_fadt(FADT_LEN, 0x32, 0);
    fadt_buf[0] = 'X';
    CU_ASSERT_EQUAL(acpi_fadt_extract((const struct acpi_sdt_header *)fadt_buf, &info), -EINVAL);
    CU_ASSERT_EQUAL(acpi_fadt_extract(NULL, &info), -EINVAL);
}

static void test_century_plausibility(void)
{
    CU_ASSERT_FALSE(acpi_century_reg_plausible(0x00));
    CU_ASSERT_FALSE(acpi_century_reg_plausible(0x0D));
    CU_ASSERT_TRUE (acpi_century_reg_plausible(0x0E));
    CU_ASSERT_TRUE (acpi_century_reg_plausible(0x32));
    CU_ASSERT_TRUE (acpi_century_reg_plausible(0x7F));
    CU_ASSERT_FALSE(acpi_century_reg_plausible(0x80));
}

static void test_fadt_implausible_century_reported_absent(void)
{
    struct acpi_fadt_info info;
    mk_fadt(FADT_LEN, 0x80, 0);
    CU_ASSERT_EQUAL(acpi_fadt_extract((const struct acpi_sdt_header *)fadt_buf, &info), 0);
    CU_ASSERT_EQUAL(info.century_reg, 0);
}

/* ── Live state from kernel_main's acpi_init() ──────────────────────────── */

static void test_live_init_succeeded(void)
{
    /* Idempotent: returns the boot-time status, so this is the boot result. */
    CU_ASSERT_EQUAL(acpi_init(mmap_get_info()), 0);
}

static void test_live_fadt_copy_valid(void)
{
    const struct acpi_sdt_header *f = acpi_fadt();
    CU_ASSERT_PTR_NOT_NULL(f);
    if (!f) {
        return;
    }
    CU_ASSERT_EQUAL(memcmp(f->signature, "FACP", 4), 0);
    CU_ASSERT_TRUE(f->length <= ACPI_FADT_COPY_MAX);
    CU_ASSERT_TRUE(acpi_checksum_ok(f, f->length));
}

static void test_live_directory(void)
{
    CU_ASSERT_TRUE(acpi_table_listed("FACP"));
    CU_ASSERT_TRUE(acpi_table_listed("APIC"));
    CU_ASSERT_FALSE(acpi_table_listed("ZZZZ"));
}

static void test_live_century_register(void)
{
    uint8_t reg = acpi_rtc_century_reg();
    CU_ASSERT_TRUE(acpi_century_reg_plausible(reg));
    CU_ASSERT_FALSE(acpi_cmos_rtc_absent());      /* QEMU has a CMOS RTC */
}

void suite_acpi_tests(CU_pSuite s)
{
    CU_add_test(s, "checksum good and bad", test_checksum_good_and_bad);
    CU_add_test(s, "checksum rejects empty", test_checksum_rejects_empty);
    CU_add_test(s, "rsdp v1 valid", test_rsdp_v1_valid);
    CU_add_test(s, "rsdp v2 valid", test_rsdp_v2_valid);
    CU_add_test(s, "rsdp bad signature", test_rsdp_bad_signature);
    CU_add_test(s, "rsdp bad checksum", test_rsdp_bad_checksum);
    CU_add_test(s, "rsdp bad extended checksum", test_rsdp_bad_extended_checksum);
    CU_add_test(s, "rsdp truncated", test_rsdp_truncated);
    CU_add_test(s, "rsdt entries and find", test_rsdt_entries_and_find);
    CU_add_test(s, "xsdt entries and find", test_xsdt_entries_and_find);
    CU_add_test(s, "xsdt entry above 4GiB skipped", test_xsdt_entry_above_4gib_skipped);
    CU_add_test(s, "xsdt entry straddling 4GiB skipped", test_xsdt_entry_straddling_4gib_skipped);
    CU_add_test(s, "root max caps output", test_root_max_caps_output);
    CU_add_test(s, "root wrong signature for kind", test_root_wrong_signature_for_kind);
    CU_add_test(s, "xsdt bad checksum rejected, rsdt still good",
                test_xsdt_bad_checksum_rejected_rsdt_still_good);
    CU_add_test(s, "root oversize length rejected", test_root_oversize_length_rejected);
    CU_add_test(s, "root undersize length rejected", test_root_undersize_length_rejected);
    CU_add_test(s, "listed table with bad checksum not found",
                test_listed_table_with_bad_checksum_not_found);
    CU_add_test(s, "fadt full length", test_fadt_full_length);
    CU_add_test(s, "fadt old revision ignores iapc", test_fadt_old_revision_ignores_iapc);
    CU_add_test(s, "fadt truncated before iapc", test_fadt_truncated_before_iapc);
    CU_add_test(s, "fadt truncated before century", test_fadt_truncated_before_century);
    CU_add_test(s, "fadt wrong signature", test_fadt_wrong_signature);
    CU_add_test(s, "century plausibility", test_century_plausibility);
    CU_add_test(s, "fadt implausible century reported absent",
                test_fadt_implausible_century_reported_absent);
    CU_add_test(s, "live init succeeded", test_live_init_succeeded);
    CU_add_test(s, "live fadt copy valid", test_live_fadt_copy_valid);
    CU_add_test(s, "live directory", test_live_directory);
    CU_add_test(s, "live century register", test_live_century_register);
}
