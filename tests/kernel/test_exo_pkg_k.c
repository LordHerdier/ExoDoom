/*
 * test_exo_pkg_k.c — SCRUM-229's acceptance test for the ExoDoom program
 * package format (src/exo_pkg.h): a unit test that round-trips a package
 * through exo_pkg_pack() -> exo_pkg_parse(), plus the rejection paths that
 * format's own comment documents.
 *
 * Pure buffer logic, no device/syscall dependency -- unlike most ring-3
 * work in this tree, this needs no probe and no mount/unmount pair, so the
 * suite is registered with (NULL, NULL) the same way e.g. "pci" is.
 */

#include "kunit.h"

#include "exo_pkg.h"

#include <stddef.h>
#include <stdint.h>

/* ---- shared fixture ----------------------------------------------------- */

static const unsigned char fixture_code[16] = {
    0x90, 0x90, 0x90, 0xC3, 0x01, 0x02, 0x03, 0x04,
    0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C,
};
static const unsigned char fixture_data[8] = {
    0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22,
};

static void fill_fixture_header(exo_pkg_header_t *hdr)
{
    hdr->magic = EXO_PKG_MAGIC;
    hdr->version = EXO_PKG_VERSION;
    hdr->code_len = sizeof(fixture_code);
    hdr->data_len = sizeof(fixture_data);
    hdr->bss_len = 4096;
    hdr->stack_pages = 1;
    hdr->params_len = 4;
    hdr->min_pages = 8;
    hdr->recommended_pages = 16;
    hdr->sound_voices = 2;
    hdr->want_flags = EXO_PKG_WANT_FRAMEBUFFER | EXO_PKG_WANT_KEYBOARD;
    hdr->reserved[0] = 0;
    hdr->reserved[1] = 0;
    hdr->reserved[2] = 0;
    hdr->reserved[3] = 0;
}

/* ---- round trip ----------------------------------------------------------
 */

static void test_round_trip_preserves_header_and_bytes(void)
{
    exo_pkg_header_t hdr;
    fill_fixture_header(&hdr);

    unsigned char buf[sizeof(exo_pkg_header_t) + sizeof(fixture_code) +
                       sizeof(fixture_data)];
    size_t out_len = 0;

    int rc = exo_pkg_pack(buf, sizeof(buf), &hdr, fixture_code, fixture_data,
                          &out_len);
    CU_ASSERT_EQUAL(rc, 0);
    CU_ASSERT_EQUAL(out_len, sizeof(buf));

    exo_pkg_header_t parsed_hdr = { 0 };
    const void *parsed_code = NULL;
    const void *parsed_data = NULL;
    rc = exo_pkg_parse(buf, out_len, &parsed_hdr, &parsed_code, &parsed_data);
    CU_ASSERT_EQUAL(rc, 0);

    CU_ASSERT_EQUAL(parsed_hdr.magic, EXO_PKG_MAGIC);
    CU_ASSERT_EQUAL(parsed_hdr.version, EXO_PKG_VERSION);
    CU_ASSERT_EQUAL(parsed_hdr.code_len, hdr.code_len);
    CU_ASSERT_EQUAL(parsed_hdr.data_len, hdr.data_len);
    CU_ASSERT_EQUAL(parsed_hdr.bss_len, hdr.bss_len);
    CU_ASSERT_EQUAL(parsed_hdr.stack_pages, hdr.stack_pages);
    CU_ASSERT_EQUAL(parsed_hdr.params_len, hdr.params_len);
    CU_ASSERT_EQUAL(parsed_hdr.min_pages, hdr.min_pages);
    CU_ASSERT_EQUAL(parsed_hdr.recommended_pages, hdr.recommended_pages);
    CU_ASSERT_EQUAL(parsed_hdr.sound_voices, hdr.sound_voices);
    CU_ASSERT_EQUAL(parsed_hdr.want_flags, hdr.want_flags);

    const unsigned char *code_bytes = (const unsigned char *)parsed_code;
    const unsigned char *data_bytes = (const unsigned char *)parsed_data;
    for (size_t i = 0; i < sizeof(fixture_code); i++)
        CU_ASSERT_EQUAL(code_bytes[i], fixture_code[i]);
    for (size_t i = 0; i < sizeof(fixture_data); i++)
        CU_ASSERT_EQUAL(data_bytes[i], fixture_data[i]);
}

static void test_round_trip_with_no_code_or_data(void)
{
    exo_pkg_header_t hdr;
    fill_fixture_header(&hdr);
    hdr.code_len = 0;
    hdr.data_len = 0;
    hdr.bss_len = 0;
    hdr.params_len = 0;

    unsigned char buf[sizeof(exo_pkg_header_t)];
    size_t out_len = 0;

    int rc = exo_pkg_pack(buf, sizeof(buf), &hdr, NULL, NULL, &out_len);
    CU_ASSERT_EQUAL(rc, 0);
    CU_ASSERT_EQUAL(out_len, sizeof(exo_pkg_header_t));

    exo_pkg_header_t parsed_hdr = { 0 };
    const void *parsed_code = (const void *)1; /* poison, must be cleared */
    const void *parsed_data = (const void *)1;
    rc = exo_pkg_parse(buf, out_len, &parsed_hdr, &parsed_code, &parsed_data);
    CU_ASSERT_EQUAL(rc, 0);
    CU_ASSERT_PTR_NULL(parsed_code);
    CU_ASSERT_PTR_NULL(parsed_data);
}

/* ---- rejection paths ----------------------------------------------------
 */

static void test_pack_rejects_bad_magic(void)
{
    exo_pkg_header_t hdr;
    fill_fixture_header(&hdr);
    hdr.magic = 0xDEADBEEFu;

    unsigned char buf[sizeof(exo_pkg_header_t) + sizeof(fixture_code) +
                       sizeof(fixture_data)];
    size_t out_len = 0;
    int rc = exo_pkg_pack(buf, sizeof(buf), &hdr, fixture_code, fixture_data,
                          &out_len);
    CU_ASSERT_EQUAL(rc, EXO_PKG_EMAGIC);
}

static void test_pack_rejects_bad_version(void)
{
    exo_pkg_header_t hdr;
    fill_fixture_header(&hdr);
    hdr.version = EXO_PKG_VERSION + 1;

    unsigned char buf[sizeof(exo_pkg_header_t) + sizeof(fixture_code) +
                       sizeof(fixture_data)];
    size_t out_len = 0;
    int rc = exo_pkg_pack(buf, sizeof(buf), &hdr, fixture_code, fixture_data,
                          &out_len);
    CU_ASSERT_EQUAL(rc, EXO_PKG_EVERSION);
}

static void test_pack_rejects_code_len_over_cap(void)
{
    exo_pkg_header_t hdr;
    fill_fixture_header(&hdr);
    hdr.code_len = EXO_PKG_MAX_CODE_PAGES * EXO_PKG_PAGE_SIZE + 1;

    unsigned char buf[sizeof(exo_pkg_header_t)];
    size_t out_len = 0;
    int rc = exo_pkg_pack(buf, sizeof(buf), &hdr, fixture_code, fixture_data,
                          &out_len);
    CU_ASSERT_EQUAL(rc, EXO_PKG_ETOOBIG);
}

static void test_pack_rejects_data_plus_bss_over_cap(void)
{
    exo_pkg_header_t hdr;
    fill_fixture_header(&hdr);
    hdr.data_len = EXO_PKG_MAX_DATA_PAGES * EXO_PKG_PAGE_SIZE;
    hdr.bss_len = 1;

    unsigned char buf[sizeof(exo_pkg_header_t)];
    size_t out_len = 0;
    int rc = exo_pkg_pack(buf, sizeof(buf), &hdr, fixture_code, fixture_data,
                          &out_len);
    CU_ASSERT_EQUAL(rc, EXO_PKG_ETOOBIG);
}

static void test_pack_rejects_stack_pages_over_cap(void)
{
    exo_pkg_header_t hdr;
    fill_fixture_header(&hdr);
    hdr.stack_pages = EXO_PKG_MAX_STACK_PAGES + 1;

    unsigned char buf[sizeof(exo_pkg_header_t) + sizeof(fixture_code) +
                       sizeof(fixture_data)];
    size_t out_len = 0;
    int rc = exo_pkg_pack(buf, sizeof(buf), &hdr, fixture_code, fixture_data,
                          &out_len);
    CU_ASSERT_EQUAL(rc, EXO_PKG_ETOOBIG);
}

static void test_pack_rejects_params_len_over_data_len(void)
{
    exo_pkg_header_t hdr;
    fill_fixture_header(&hdr);
    hdr.params_len = hdr.data_len + 1;

    unsigned char buf[sizeof(exo_pkg_header_t) + sizeof(fixture_code) +
                       sizeof(fixture_data)];
    size_t out_len = 0;
    int rc = exo_pkg_pack(buf, sizeof(buf), &hdr, fixture_code, fixture_data,
                          &out_len);
    CU_ASSERT_EQUAL(rc, EXO_PKG_ETOOBIG);
}

static void test_pack_rejects_buf_too_small(void)
{
    exo_pkg_header_t hdr;
    fill_fixture_header(&hdr);

    unsigned char buf[sizeof(exo_pkg_header_t)]; /* no room for code/data */
    size_t out_len = 0;
    int rc = exo_pkg_pack(buf, sizeof(buf), &hdr, fixture_code, fixture_data,
                          &out_len);
    CU_ASSERT_EQUAL(rc, EXO_PKG_ESHORT);
}

static void test_parse_rejects_buf_shorter_than_header(void)
{
    unsigned char buf[4] = { 0, 0, 0, 0 };
    exo_pkg_header_t parsed_hdr;
    const void *parsed_code = NULL;
    const void *parsed_data = NULL;
    int rc = exo_pkg_parse(buf, sizeof(buf), &parsed_hdr, &parsed_code,
                           &parsed_data);
    CU_ASSERT_EQUAL(rc, EXO_PKG_ESHORT);
}

static void test_parse_rejects_buf_shorter_than_declared_lengths(void)
{
    exo_pkg_header_t hdr;
    fill_fixture_header(&hdr);

    unsigned char buf[sizeof(exo_pkg_header_t) + sizeof(fixture_code) +
                       sizeof(fixture_data)];
    size_t out_len = 0;
    int rc = exo_pkg_pack(buf, sizeof(buf), &hdr, fixture_code, fixture_data,
                          &out_len);
    CU_ASSERT_EQUAL(rc, 0);

    exo_pkg_header_t parsed_hdr;
    const void *parsed_code = NULL;
    const void *parsed_data = NULL;
    /* Claim fewer bytes are available than the header itself declares. */
    rc = exo_pkg_parse(buf, out_len - 1, &parsed_hdr, &parsed_code,
                       &parsed_data);
    CU_ASSERT_EQUAL(rc, EXO_PKG_ESHORT);
}

static void test_parse_rejects_bad_magic(void)
{
    exo_pkg_header_t hdr;
    fill_fixture_header(&hdr);

    unsigned char buf[sizeof(exo_pkg_header_t) + sizeof(fixture_code) +
                       sizeof(fixture_data)];
    size_t out_len = 0;
    int rc = exo_pkg_pack(buf, sizeof(buf), &hdr, fixture_code, fixture_data,
                          &out_len);
    CU_ASSERT_EQUAL(rc, 0);

    /* Corrupt the magic in place, after a valid pack. */
    exo_pkg_header_t *corrupt = (exo_pkg_header_t *)buf;
    corrupt->magic = 0;

    exo_pkg_header_t parsed_hdr;
    const void *parsed_code = NULL;
    const void *parsed_data = NULL;
    rc = exo_pkg_parse(buf, out_len, &parsed_hdr, &parsed_code, &parsed_data);
    CU_ASSERT_EQUAL(rc, EXO_PKG_EMAGIC);
}

static void test_parse_rejects_bad_version(void)
{
    exo_pkg_header_t hdr;
    fill_fixture_header(&hdr);

    unsigned char buf[sizeof(exo_pkg_header_t) + sizeof(fixture_code) +
                       sizeof(fixture_data)];
    size_t out_len = 0;
    int rc = exo_pkg_pack(buf, sizeof(buf), &hdr, fixture_code, fixture_data,
                          &out_len);
    CU_ASSERT_EQUAL(rc, 0);

    exo_pkg_header_t *corrupt = (exo_pkg_header_t *)buf;
    corrupt->version = EXO_PKG_VERSION + 1;

    exo_pkg_header_t parsed_hdr;
    const void *parsed_code = NULL;
    const void *parsed_data = NULL;
    rc = exo_pkg_parse(buf, out_len, &parsed_hdr, &parsed_code, &parsed_data);
    CU_ASSERT_EQUAL(rc, EXO_PKG_EVERSION);
}

/* ---- registration -------------------------------------------------------
 */

void suite_exo_pkg_tests(CU_pSuite s)
{
    CU_add_test(s, "pack -> parse round-trips header, code and data bytes",
                test_round_trip_preserves_header_and_bytes);
    CU_add_test(s, "pack -> parse round-trips a package with no code/data",
                test_round_trip_with_no_code_or_data);

    CU_add_test(s, "pack rejects a bad magic", test_pack_rejects_bad_magic);
    CU_add_test(s, "pack rejects a bad version",
                test_pack_rejects_bad_version);
    CU_add_test(s, "pack rejects code_len over the loader's cap",
                test_pack_rejects_code_len_over_cap);
    CU_add_test(s, "pack rejects data_len + bss_len over the loader's cap",
                test_pack_rejects_data_plus_bss_over_cap);
    CU_add_test(s, "pack rejects stack_pages over the loader's cap",
                test_pack_rejects_stack_pages_over_cap);
    CU_add_test(s, "pack rejects params_len bigger than data_len",
                test_pack_rejects_params_len_over_data_len);
    CU_add_test(s, "pack rejects a destination buffer too small",
                test_pack_rejects_buf_too_small);

    CU_add_test(s, "parse rejects a buffer shorter than the header",
                test_parse_rejects_buf_shorter_than_header);
    CU_add_test(s,
                "parse rejects a buffer shorter than its own declared "
                "lengths",
                test_parse_rejects_buf_shorter_than_declared_lengths);
    CU_add_test(s, "parse rejects a corrupted magic",
                test_parse_rejects_bad_magic);
    CU_add_test(s, "parse rejects a corrupted version",
                test_parse_rejects_bad_version);
}
