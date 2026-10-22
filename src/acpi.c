/*
 * acpi.c — read-only ACPI table parser (SCRUM-250).
 *
 * See acpi.h for the scope and, above all, the ordering rule: acpi_init()
 * runs before vmm_init(), because the kernel map does not map the memory the
 * firmware tables live in. Everything callers need afterwards is copied into
 * the statics below.
 */

#include "acpi.h"
#include "errno.h"
#include "serial.h"
#include "string.h"

#define ACPI_4GIB   0x100000000ULL

/* Legacy BIOS area searched for the RSDP when the bootloader gave us no tag.
 * The EBDA is deliberately not searched: its segment pointer is at 0x40E, in
 * page 0, which is the unmapped NULL guard. */
#define BIOS_AREA_START  0xE0000u
#define BIOS_AREA_END    0x100000u

/* ── Pure functions ─────────────────────────────────────────────────────── */

bool acpi_checksum_ok(const void *p, size_t len)
{
    if (!p || len == 0) {
        return false;
    }
    const uint8_t *b = (const uint8_t *)p;
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum = (uint8_t)(sum + b[i]);
    }
    return sum == 0;
}

const struct acpi_rsdp *acpi_rsdp_validate(const void *p, size_t avail)
{
    if (!p || avail < ACPI_RSDP_V1_SIZE) {
        return NULL;
    }
    const struct acpi_rsdp *r = (const struct acpi_rsdp *)p;
    if (memcmp(r->signature, "RSD PTR ", 8) != 0) {
        return NULL;
    }
    if (!acpi_checksum_ok(p, ACPI_RSDP_V1_SIZE)) {
        return NULL;
    }
    if (r->revision >= 2) {
        if (avail < sizeof(struct acpi_rsdp)) {
            return NULL;
        }
        uint32_t len = r->length;
        if (len < sizeof(struct acpi_rsdp) || len > avail) {
            return NULL;
        }
        if (!acpi_checksum_ok(p, len)) {
            return NULL;
        }
    }
    return r;
}

/* Can a table header at physical address `a` be read at all? Only the boot
 * identity map (the first 4 GiB) is live while acpi_init() runs, so the whole
 * header, not just its first byte, has to fit below 4 GiB. */
static bool header_in_reach(uint64_t a)
{
    return a != 0 && a + sizeof(struct acpi_sdt_header) <= ACPI_4GIB;
}

/* Header sanity shared by the root table and every table it lists: length in
 * bounds, the whole table below 4 GiB, and checksum good. Does not look at the
 * signature. The caller has already established header_in_reach(). */
static bool sdt_sane(const struct acpi_sdt_header *h)
{
    uint32_t len = h->length;
    if (len < sizeof(struct acpi_sdt_header) || len > ACPI_MAX_TABLE_LEN) {
        return false;
    }
    if ((uint64_t)(uintptr_t)h + len > ACPI_4GIB) {
        return false;
    }
    return acpi_checksum_ok(h, len);
}

/* Validates the root and returns its entry count, or -EINVAL. */
static int root_count(const struct acpi_sdt_header *root, bool is_xsdt)
{
    if (!root) {
        return -EINVAL;
    }
    if (memcmp(root->signature, is_xsdt ? "XSDT" : "RSDT", ACPI_SIG_LEN) != 0) {
        return -EINVAL;
    }
    if (!sdt_sane(root)) {
        return -EINVAL;
    }
    uint32_t width = is_xsdt ? 8u : 4u;
    return (int)((root->length - sizeof(struct acpi_sdt_header)) / width);
}

/* Entry i of a root already validated by root_count(). XSDT entries are only
 * 4-byte aligned, hence the memcpy. */
static uint64_t root_entry(const struct acpi_sdt_header *root, bool is_xsdt, int i)
{
    const uint8_t *base = (const uint8_t *)root + sizeof(struct acpi_sdt_header);
    if (is_xsdt) {
        uint64_t v;
        memcpy(&v, base + (size_t)i * 8, 8);
        return v;
    }
    uint32_t v32;
    memcpy(&v32, base + (size_t)i * 4, 4);
    return v32;
}

int acpi_root_entries(const struct acpi_sdt_header *root, bool is_xsdt,
                      uint64_t *out, int max)
{
    int n = root_count(root, is_xsdt);
    if (n < 0) {
        return n;
    }
    int written = 0;
    for (int i = 0; i < n && written < max; i++) {
        uint64_t a = root_entry(root, is_xsdt, i);
        if (!header_in_reach(a)) {
            continue;
        }
        out[written++] = a;
    }
    return written;
}

const struct acpi_sdt_header *acpi_root_find(const struct acpi_sdt_header *root,
                                             bool is_xsdt, const char sig[4])
{
    int n = root_count(root, is_xsdt);
    for (int i = 0; i < n; i++) {
        uint64_t a = root_entry(root, is_xsdt, i);
        if (!header_in_reach(a)) {
            continue;
        }
        const struct acpi_sdt_header *h =
            (const struct acpi_sdt_header *)(uintptr_t)a;
        if (memcmp(h->signature, sig, ACPI_SIG_LEN) == 0 && sdt_sane(h)) {
            return h;
        }
    }
    return NULL;
}

bool acpi_century_reg_plausible(uint8_t v)
{
    return v >= 0x0E && v < 0x80;
}

int acpi_fadt_extract(const struct acpi_sdt_header *fadt,
                      struct acpi_fadt_info *out)
{
    if (!fadt || !out) {
        return -EINVAL;
    }
    if (memcmp(fadt->signature, "FACP", ACPI_SIG_LEN) != 0) {
        return -EINVAL;
    }
    uint32_t len = fadt->length;
    if (len < sizeof(struct acpi_sdt_header) || len > ACPI_MAX_TABLE_LEN) {
        return -EINVAL;
    }
    const uint8_t *b = (const uint8_t *)fadt;

    out->century_reg = 0;
    if (len > ACPI_FADT_CENTURY_OFF) {
        uint8_t c = b[ACPI_FADT_CENTURY_OFF];
        out->century_reg = acpi_century_reg_plausible(c) ? c : 0;
    }
    out->iapc_boot_arch = 0;
    /* IAPC_BOOT_ARCH exists from FADT revision 3 (ACPI 2.0); before that the
     * bytes are reserved and may not be zero. */
    if (fadt->revision >= 3 && len >= ACPI_FADT_IAPC_OFF + 2u) {
        uint16_t v;
        memcpy(&v, b + ACPI_FADT_IAPC_OFF, 2);
        out->iapc_boot_arch = v;
    }
    return 0;
}

/* ── Kernel state ───────────────────────────────────────────────────────── */

static bool    g_inited;
static int     g_status;
static uint8_t g_fadt_copy[ACPI_FADT_COPY_MAX] __attribute__((aligned(8)));
static bool    g_have_fadt;
static struct acpi_fadt_info g_info;
static char    g_dir[ACPI_DIR_MAX][ACPI_SIG_LEN];
static int     g_dir_n;

/* Locate a validated RSDP: Multiboot2 tag first (new, then old), then the BIOS
 * area scan. */
static const struct acpi_rsdp *find_rsdp(const struct mb2_info *mb)
{
    if (mb) {
        static const uint32_t tags[] = { MB2_TAG_ACPI_NEW, MB2_TAG_ACPI_OLD };
        for (unsigned i = 0; i < sizeof(tags) / sizeof(tags[0]); i++) {
            const struct mb2_tag *t = mb2_find_tag(mb, tags[i]);
            if (t && t->size > sizeof(struct mb2_tag)) {
                const struct mb2_tag_acpi *at = (const struct mb2_tag_acpi *)t;
                const struct acpi_rsdp *r =
                    acpi_rsdp_validate(at->rsdp, t->size - sizeof(struct mb2_tag));
                if (r) {
                    return r;
                }
            }
        }
    }
    for (uintptr_t a = BIOS_AREA_START; a + 16 <= BIOS_AREA_END; a += 16) {
        if (memcmp((const void *)a, "RSD PTR ", 8) != 0) {
            continue;
        }
        size_t avail = BIOS_AREA_END - a;
        const struct acpi_rsdp *r = acpi_rsdp_validate((const void *)a, avail);
        if (r) {
            return r;
        }
    }
    return NULL;
}

/* Try one root table: it must validate and contain a usable FADT. Everything
 * is built in locals and committed to the statics only once every step has
 * succeeded, so a failed attempt (and the XSDT-then-RSDT fallback) never leaves
 * half-built state behind. */
static int try_root(uint64_t addr, bool is_xsdt)
{
    if (!header_in_reach(addr)) {
        return -EINVAL;
    }
    const struct acpi_sdt_header *root =
        (const struct acpi_sdt_header *)(uintptr_t)addr;

    const struct acpi_sdt_header *fadt = acpi_root_find(root, is_xsdt, "FACP");
    if (!fadt) {
        return -ENOENT;
    }
    if (fadt->length > ACPI_FADT_COPY_MAX) {
        return -ERANGE;
    }
    struct acpi_fadt_info info;
    if (acpi_fadt_extract(fadt, &info) != 0) {
        return -EINVAL;
    }

    uint64_t addrs[ACPI_DIR_MAX];
    char dir[ACPI_DIR_MAX][ACPI_SIG_LEN];
    int dir_n = 0;
    int n = acpi_root_entries(root, is_xsdt, addrs, ACPI_DIR_MAX);
    for (int i = 0; i < n; i++) {
        const struct acpi_sdt_header *h =
            (const struct acpi_sdt_header *)(uintptr_t)addrs[i];
        if (!sdt_sane(h)) {
            /* Not silent: a table dropped here is indistinguishable from an
             * absent one to acpi_table_listed(). */
            serial_print("ACPI: skipping table '");
            for (int c = 0; c < ACPI_SIG_LEN; c++) {
                serial_putc(h->signature[c]);
            }
            serial_print("' (bad length or checksum)\n");
            continue;
        }
        memcpy(dir[dir_n++], h->signature, ACPI_SIG_LEN);
    }

    memcpy(g_fadt_copy, fadt, fadt->length);
    g_info = info;
    memcpy(g_dir, dir, sizeof(dir));
    g_dir_n = dir_n;
    g_have_fadt = true;
    return 0;
}

int acpi_init(const struct mb2_info *mb)
{
    if (g_inited) {
        return g_status;
    }
    g_inited = true;
    g_have_fadt = false;
    g_dir_n = 0;
    memset(&g_info, 0, sizeof(g_info));

    const struct acpi_rsdp *rsdp = find_rsdp(mb);
    if (!rsdp) {
        serial_print("ACPI: no RSDP found (no multiboot tag, none in BIOS area)\n");
        g_status = -ENODEV;
        return g_status;
    }

    int rc = -EINVAL;
    bool via_xsdt = false;
    if (rsdp->revision >= 2 && rsdp->xsdt_addr != 0) {
        rc = try_root(rsdp->xsdt_addr, true);
        via_xsdt = (rc == 0);
    }
    if (rc != 0) {
        rc = try_root(rsdp->rsdt_addr, false);
    }
    if (rc != 0) {
        serial_print("ACPI: no usable root table / FADT\n");
        g_status = rc;
        return g_status;
    }

    serial_print("ACPI: rev ");
    serial_print_u32(rsdp->revision);
    serial_print(via_xsdt ? " via XSDT, " : " via RSDT, ");
    serial_print_u32((uint32_t)g_dir_n);
    serial_print(" tables, century reg 0x");
    serial_print_hex(g_info.century_reg);
    serial_print(g_info.iapc_boot_arch & ACPI_IAPC_NO_CMOS_RTC
                     ? ", no CMOS RTC\n" : "\n");

    g_status = 0;
    return 0;
}

bool acpi_table_listed(const char sig[4])
{
    for (int i = 0; i < g_dir_n; i++) {
        if (memcmp(g_dir[i], sig, ACPI_SIG_LEN) == 0) {
            return true;
        }
    }
    return false;
}

const struct acpi_sdt_header *acpi_fadt(void)
{
    return g_have_fadt ? (const struct acpi_sdt_header *)g_fadt_copy : NULL;
}

uint8_t acpi_rtc_century_reg(void)
{
    return g_have_fadt ? g_info.century_reg : 0;
}

bool acpi_cmos_rtc_absent(void)
{
    return g_have_fadt && (g_info.iapc_boot_arch & ACPI_IAPC_NO_CMOS_RTC);
}
