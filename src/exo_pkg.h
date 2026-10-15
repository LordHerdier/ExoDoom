#ifndef EXO_PKG_H
#define EXO_PKG_H

/*
 * exo_pkg — the on-disk ExoDoom program package format (SCRUM-229).
 *
 * SCRUM-228 ("Program Install & Packaging") needs a container for an
 * installable program: today every LibOS app is a blob baked into the
 * kernel image by build_ring3_link_target() (docker/scripts/build.sh) and
 * launched from a linker symbol via exo_launch (#21, src/syscall_launch.c).
 * This header defines the format that container takes once it lives on
 * disk instead -- a fixed header, magic + version first, followed by the
 * code bytes and then the data bytes. It carries exactly what
 * libos_build_image() (src/libos_launch.h) currently gets from linker
 * symbols (code/data/bss lengths, stack page count) plus the resource
 * manifest SCRUM-232's admission check needs (minimum/recommended pages,
 * which of the framebuffer/disk/keyboard it wants, how many sound voices).
 *
 * This file defines the format ONLY. The loader that builds a libos_image_t
 * from a parsed package is SCRUM-230; the host-side tool that produces a
 * package from an existing ring-3 link target's objects is SCRUM-231. Both
 * of those include this header rather than each defining their own copy of
 * the struct, which is why it is deliberately header-only and restricted to
 * <stdint.h>/<stddef.h>: SCRUM-231's packer is a normal hosted-C command
 * line tool, not a ring-3 link target, so it cannot pull in anything
 * freestanding or kernel-only (unlike e.g. src/libos_fs/exofs_blockdev.c's
 * #ifdef EXO_KERNEL seam, which still assumes a kernel-or-ring-3 compile --
 * a third, host-only context needs the simpler answer of "depend on
 * nothing" instead of a third branch on that ifdef).
 *
 * bss is recorded as a LENGTH, not bytes -- there is nothing to store, the
 * loader just needs to know how much to zero. Same precedent as
 * tests/kernel/libos_c_probe/libos_c_probe_layout.h's generated
 * *_BSS_LEN macro and src/libos_snake/libos_snake_layout.h's
 * LIBOS_SNAKE_BSS_LEN.
 *
 * exo_pkg_pack()/exo_pkg_parse() are zero-copy/zero-allocation, the same
 * ethos src/doom_dmx.c's doom_dmx_t uses for WAD lumps: parse() points
 * out_code/out_data into the caller's own buffer rather than copying, since
 * there is no reason to duplicate a package that may already be a
 * memory-mapped disk read.
 */

#include <stddef.h>
#include <stdint.h>

#define EXO_PKG_MAGIC   0x4B504F45u /* "EOPK" read as a little-endian u32 */
#define EXO_PKG_VERSION 1u

/* Resource manifest want-flags (src/exo_pkg.h, SCRUM-229/232). Bitmask in
 * exo_pkg_header_t.want_flags -- what the admission check in SCRUM-232
 * reads to decide whether to grant the framebuffer/disk binding/keyboard
 * ring, alongside min_pages/recommended_pages/sound_voices below. */
#define EXO_PKG_WANT_FRAMEBUFFER (1u << 0)
#define EXO_PKG_WANT_DISK        (1u << 1)
#define EXO_PKG_WANT_KEYBOARD    (1u << 2)

/* Negative sentinel returns for exo_pkg_pack()/exo_pkg_parse(). Deliberately
 * NOT src/exo_errno.h's -EXO_E* space: that header is kernel/LibOS-only and
 * this code also runs on the host (SCRUM-231's packer). */
#define EXO_PKG_EMAGIC   (-1) /* header.magic is not EXO_PKG_MAGIC */
#define EXO_PKG_EVERSION (-2) /* header.version is not EXO_PKG_VERSION */
#define EXO_PKG_ETOOBIG  (-3) /* a length exceeds the loader's fixed caps,
                                * or the destination buffer is too small */
#define EXO_PKG_ESHORT   (-4) /* buf_len is too small to hold even the
                                * header, or less than header + the lengths
                                * the header itself declares */

/* Mirrors src/libos_launch.h's LIBOS_LAUNCH_MAX_CODE_PAGES/
 * LIBOS_LAUNCH_MAX_DATA_PAGES/LIBOS_LAUNCH_STACK_PAGES -- this format
 * answers to that loader's fixed caps, so it checks against them rather
 * than inventing its own. Restated here as plain integers (not by
 * #including libos_launch.h) because that header pulls in exo_syscall.h,
 * which is kernel/LibOS-only; keeping exo_pkg.h host-includable matters
 * more than deduplicating three constants, and a _Static_assert in
 * src/libos_launch.h itself (see its own comment) is what would catch the
 * two drifting apart.
 */
#define EXO_PKG_MAX_CODE_PAGES  192u
#define EXO_PKG_MAX_DATA_PAGES  192u
#define EXO_PKG_MAX_STACK_PAGES 16u
#define EXO_PKG_PAGE_SIZE       0x1000u

typedef struct {
    uint32_t magic;   /* EXO_PKG_MAGIC */
    uint32_t version; /* EXO_PKG_VERSION */

    /* What libos_build_image() (src/libos_launch.h) currently gets from
     * linker symbols. */
    uint32_t code_len;
    uint32_t data_len;
    uint32_t bss_len;
    uint32_t stack_pages;
    uint32_t params_len; /* size of the first global in .data -- the struct
                           * libos_launch_patch_params() overwrites in place
                           * (SCRUM-175's convention) -- 0 if this program
                           * takes no launch params. */

    /* Resource manifest (SCRUM-232 reads these at admission time). */
    uint32_t min_pages;
    uint32_t recommended_pages;
    uint32_t sound_voices; /* voices wanted, 0 = none */
    uint32_t want_flags;   /* EXO_PKG_WANT_* bitmask */

    uint32_t reserved[4]; /* must be zero; keeps the header growable without
                            * bumping EXO_PKG_VERSION for every new field */
} exo_pkg_header_t;

/*
 * Validate `*hdr` and write header + `code_len` bytes of `code` +
 * `data_len` bytes of `data` into `buf`, in that order, with no padding
 * between sections.
 *
 * Rejects (before writing anything):
 *   - EXO_PKG_EMAGIC / EXO_PKG_EVERSION if hdr->magic/version aren't what
 *     this header defines -- callers fill those two fields in themselves
 *     (same struct, not an implicit side effect of calling pack), so this
 *     catches a caller building against a stale copy of the struct.
 *   - EXO_PKG_ETOOBIG if hdr->code_len > EXO_PKG_MAX_CODE_PAGES*PAGE_SIZE,
 *     if hdr->data_len + hdr->bss_len > EXO_PKG_MAX_DATA_PAGES*PAGE_SIZE,
 *     if hdr->stack_pages > EXO_PKG_MAX_STACK_PAGES, or if hdr->params_len
 *     is nonzero and exceeds hdr->data_len (the params struct cannot be
 *     bigger than the data section it lives in).
 *   - EXO_PKG_ESHORT if buf_cap < sizeof(*hdr) + hdr->code_len +
 *     hdr->data_len.
 *
 * code may be NULL only if code_len is 0; same for data/data_len. On
 * success, writes the total package length (sizeof(*hdr) + code_len +
 * data_len) to *out_len and returns 0.
 */
static inline int exo_pkg_pack(void *buf, size_t buf_cap,
                               const exo_pkg_header_t *hdr,
                               const void *code, const void *data,
                               size_t *out_len) {
    if (hdr->magic != EXO_PKG_MAGIC)
        return EXO_PKG_EMAGIC;
    if (hdr->version != EXO_PKG_VERSION)
        return EXO_PKG_EVERSION;

    if (hdr->code_len > EXO_PKG_MAX_CODE_PAGES * EXO_PKG_PAGE_SIZE)
        return EXO_PKG_ETOOBIG;
    if ((uint64_t)hdr->data_len + (uint64_t)hdr->bss_len >
        (uint64_t)EXO_PKG_MAX_DATA_PAGES * EXO_PKG_PAGE_SIZE)
        return EXO_PKG_ETOOBIG;
    if (hdr->stack_pages > EXO_PKG_MAX_STACK_PAGES)
        return EXO_PKG_ETOOBIG;
    if (hdr->params_len > hdr->data_len)
        return EXO_PKG_ETOOBIG;

    size_t total = sizeof(*hdr) + (size_t)hdr->code_len + (size_t)hdr->data_len;
    if (buf_cap < total)
        return EXO_PKG_ESHORT;

    unsigned char *out = (unsigned char *)buf;
    exo_pkg_header_t *out_hdr = (exo_pkg_header_t *)out;
    *out_hdr = *hdr;
    out += sizeof(*hdr);

    if (hdr->code_len > 0) {
        const unsigned char *src = (const unsigned char *)code;
        for (uint32_t i = 0; i < hdr->code_len; i++)
            out[i] = src[i];
        out += hdr->code_len;
    }

    if (hdr->data_len > 0) {
        const unsigned char *src = (const unsigned char *)data;
        for (uint32_t i = 0; i < hdr->data_len; i++)
            out[i] = src[i];
    }

    *out_len = total;
    return 0;
}

/*
 * Validate the package at `buf` (`buf_len` bytes available) and point
 * `*out_code`/`*out_data` at the code/data sections inside it -- no copy.
 * Writes the parsed header to `*out_hdr` regardless of where in `buf` it
 * physically sits.
 *
 * Rejects the same way exo_pkg_pack() does, plus:
 *   - EXO_PKG_ESHORT if buf_len < sizeof(exo_pkg_header_t) (can't even read
 *     the header), or if buf_len < sizeof(header) + code_len + data_len
 *     once the header has been read (the buffer is shorter than what the
 *     header itself claims it holds).
 *
 * On success, *out_code is NULL iff code_len == 0, and likewise for
 * *out_data/data_len.
 */
static inline int exo_pkg_parse(const void *buf, size_t buf_len,
                                exo_pkg_header_t *out_hdr,
                                const void **out_code,
                                const void **out_data) {
    if (buf_len < sizeof(exo_pkg_header_t))
        return EXO_PKG_ESHORT;

    const unsigned char *in = (const unsigned char *)buf;
    exo_pkg_header_t hdr;
    for (size_t i = 0; i < sizeof(hdr); i++)
        ((unsigned char *)&hdr)[i] = in[i];

    if (hdr.magic != EXO_PKG_MAGIC)
        return EXO_PKG_EMAGIC;
    if (hdr.version != EXO_PKG_VERSION)
        return EXO_PKG_EVERSION;

    if (hdr.code_len > EXO_PKG_MAX_CODE_PAGES * EXO_PKG_PAGE_SIZE)
        return EXO_PKG_ETOOBIG;
    if ((uint64_t)hdr.data_len + (uint64_t)hdr.bss_len >
        (uint64_t)EXO_PKG_MAX_DATA_PAGES * EXO_PKG_PAGE_SIZE)
        return EXO_PKG_ETOOBIG;
    if (hdr.stack_pages > EXO_PKG_MAX_STACK_PAGES)
        return EXO_PKG_ETOOBIG;
    if (hdr.params_len > hdr.data_len)
        return EXO_PKG_ETOOBIG;

    size_t total = sizeof(hdr) + (size_t)hdr.code_len + (size_t)hdr.data_len;
    if (buf_len < total)
        return EXO_PKG_ESHORT;

    *out_hdr = hdr;

    const unsigned char *p = in + sizeof(hdr);
    *out_code = (hdr.code_len > 0) ? (const void *)p : NULL;
    p += hdr.code_len;
    *out_data = (hdr.data_len > 0) ? (const void *)p : NULL;

    return 0;
}

#endif /* EXO_PKG_H */
