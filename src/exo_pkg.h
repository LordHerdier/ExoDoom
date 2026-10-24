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
#define EXO_PKG_WANT_WAD         (1u << 3) /* mount the IWAD and patch the program's
                                            * first .data global (params_len must be
                                            * sizeof(libos_wad_params_t), 16), SCRUM-232 */

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
#define EXO_PKG_EINVAL   (-5) /* a manifest field is malformed: non-zero
                                * reserved[], min_pages > recommended_pages,
                                * stack_pages == 0, sound_voices over
                                * EXO_PKG_MAX_SOUND_VOICES, or an unknown
                                * want_flags bit */

/* Mirrors src/libos_launch.h's LIBOS_LAUNCH_MAX_CODE_PAGES/
 * LIBOS_LAUNCH_MAX_DATA_PAGES/LIBOS_LAUNCH_STACK_PAGES -- this format
 * answers to that loader's fixed caps, so it checks against them rather
 * than inventing its own. Restated here as plain integers (not by
 * #including libos_launch.h) because that header pulls in exo_syscall.h,
 * which is kernel/LibOS-only; keeping exo_pkg.h host-includable matters
 * more than deduplicating three constants. src/libos_launch.h includes
 * this header and _Static_asserts the two sets equal, which is what catches
 * them drifting apart.
 */
#define EXO_PKG_MAX_CODE_PAGES  192u
#define EXO_PKG_MAX_DATA_PAGES  192u
#define EXO_PKG_MAX_STACK_PAGES 16u
#define EXO_PKG_PAGE_SIZE       0x1000u

/* Restates src/pcm_mixer.h's PCM_MIXER_VOICES for the same reason as the
 * page caps above; tests/kernel/test_exo_pkg_k.c, which sees both headers,
 * asserts they agree. */
#define EXO_PKG_MAX_SOUND_VOICES 8u

/* Every want_flags bit this version defines; anything else is rejected so a
 * v1 parser never silently accepts a request it does not understand. */
#define EXO_PKG_WANT_ALL (EXO_PKG_WANT_FRAMEBUFFER | EXO_PKG_WANT_DISK | \
                          EXO_PKG_WANT_KEYBOARD | \
                          EXO_PKG_WANT_WAD)

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
                            * bumping EXO_PKG_VERSION for every new field --
                            * which only holds because v1 pack/parse reject
                            * a non-zero value (EXO_PKG_EINVAL), so a future
                            * package that uses one cannot be mistaken for v1 */
} exo_pkg_header_t;

/* On-disk layout: 15 u32 fields (11 named + reserved[4]), no padding. Changing this is a format
 * change and needs an EXO_PKG_VERSION bump. */
_Static_assert(sizeof(exo_pkg_header_t) == 60,
               "exo_pkg_header_t is an on-disk layout");

/* Pages a launch takes from the PMM before the program runs a single
 * instruction (SCRUM-232): the code, the data+bss, the fixed stack and the
 * page-table pages libos_build_image() builds around them. This is what
 * min_pages must at least be -- a manifest below it admits a program that
 * then fails inside the loader, which is the failure the admission check
 * exists to replace. Shared so the packer (tools/exopkg) and the kernel test
 * that measures a real launch against it use one formula.
 *
 * The stack is EXO_PKG_MAX_STACK_PAGES, not the header's stack_pages: the
 * builder always maps LIBOS_LAUNCH_STACK_PAGES (== the max), and stack_pages
 * is advisory until it takes a count (docs/architecture.md SS7). */
#define EXO_PKG_TABLE_PAGES 4u /* PML4 + PDPT + PD + PT; measured, see
                                * tests/kernel/test_launch_pkg_k.c */
/* EXO_PKG_WANT_WAD adds the IWAD mapping (src/libos_wad_map.h): the WAD's
 * pages are shared, but the page tables over them are the program's own, one
 * PT per 2 MiB. The manifest cannot know the WAD's size, so this is the
 * worst case -- LIBOS_WAD_MAX_BYTES (64 MiB) / 2 MiB, plus one for an
 * unaligned start -- which keeps min_pages a true floor. */
#define EXO_PKG_WAD_TABLE_PAGES 33u

static inline uint32_t exo_pkg_pages(uint64_t bytes)
{
    return (uint32_t)((bytes + EXO_PKG_PAGE_SIZE - 1) / EXO_PKG_PAGE_SIZE);
}

static inline uint32_t exo_pkg_footprint_pages(uint32_t code_len,
                                               uint32_t data_len,
                                               uint32_t bss_len,
                                               uint32_t want_flags)
{
    return exo_pkg_pages(code_len) +
           exo_pkg_pages((uint64_t)data_len + bss_len) +
           EXO_PKG_MAX_STACK_PAGES + EXO_PKG_TABLE_PAGES +
           ((want_flags & EXO_PKG_WANT_WAD) ? EXO_PKG_WAD_TABLE_PAGES : 0u);
}

/* Everything pack and parse both check about a header, in one place so the
 * two cannot disagree on what a valid package is. Returns 0 or an
 * EXO_PKG_E* code; says nothing about buffer lengths. */
static inline int exo_pkg_check_header(const exo_pkg_header_t *hdr) {
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

    for (int i = 0; i < 4; i++)
        if (hdr->reserved[i] != 0)
            return EXO_PKG_EINVAL;
    if (hdr->stack_pages < 1)
        return EXO_PKG_EINVAL;
    if (hdr->min_pages > hdr->recommended_pages)
        return EXO_PKG_EINVAL;
    if (hdr->sound_voices > EXO_PKG_MAX_SOUND_VOICES)
        return EXO_PKG_EINVAL;
    if (hdr->want_flags & ~EXO_PKG_WANT_ALL)
        return EXO_PKG_EINVAL;

    return 0;
}

/*
 * Validate `*hdr` and write header + `code_len` bytes of `code` +
 * `data_len` bytes of `data` into `buf`, in that order, with no padding
 * between sections.
 *
 * Rejects (before writing anything):
 *   - EXO_PKG_EINVAL for a malformed manifest (see exo_pkg_check_header()).
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
    int rc = exo_pkg_check_header(hdr);
    if (rc != 0)
        return rc;

    size_t total = sizeof(*hdr) + (size_t)hdr->code_len + (size_t)hdr->data_len;
    if (buf_cap < total)
        return EXO_PKG_ESHORT;

    unsigned char *out = (unsigned char *)buf;
    /* Byte copy, not a cast: buf may be an unaligned view of a disk read. */
    const unsigned char *hsrc = (const unsigned char *)hdr;
    for (size_t i = 0; i < sizeof(*hdr); i++)
        out[i] = hsrc[i];
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

    int rc = exo_pkg_check_header(&hdr);
    if (rc != 0)
        return rc;

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
