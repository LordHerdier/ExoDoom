/*
 * exopkg -- host-side packer for the ExoDoom program package format
 * (src/exo_pkg.h, SCRUM-229). SCRUM-230 needs one real package on the disk
 * image to prove the loader (a program whose blob is NOT linked into the
 * kernel still runs); this is the minimal tool that makes it. SCRUM-231
 * generalises the invocation to every ring-3 link target.
 *
 * Input is exactly what build_ring3_link_target() (docker/scripts/build.sh)
 * already leaves in build/: <name>_code.bin and <name>_data.bin, plus the
 * .bss length it reads out of the linked ELF. Nothing here is a second
 * implementation of the format -- it fills an exo_pkg_header_t and calls
 * exo_pkg_pack(), so a layout or validity rule changing in src/exo_pkg.h
 * changes this tool with it.
 *
 * Hosted C, not freestanding: compiled with the build host's gcc and,
 * like tools/mkexofsimg, deliberately NOT given "-I src" (that would shadow
 * <stdio.h>/<stdlib.h> with the project's shims). It reaches exo_pkg.h by
 * the explicit path on its own include line, and that header depends on
 * nothing but <stddef.h>/<stdint.h>.
 *
 * usage: exopkg <out.exo> <code.bin> <data.bin> <bss-len> <want> [options]
 *        exopkg --check <package.exo>
 *   <want> is a comma-separated list of: fb, disk, kbd, wad, progvol, or "none".
 *   "progvol" (SCRUM-75) asks for read-only access to the program volume.
 *   "wad" (SCRUM-232) asks the kernel to map the IWAD and patch the first
 *   .data global; it needs --params-len 16.
 *   options (SCRUM-231; each defaults to the derived value described above):
 *     --params-len N          size of the first-global launch params struct
 *     --sound-voices N        PCM mixer voices wanted
 *     --min-pages N           admission minimum (default: exo_pkg_footprint_pages())
 *     --recommended-pages N   admission recommendation (default: min + slack)
 *     --stack-pages N         stack pages (default: EXO_PKG_MAX_STACK_PAGES)
 *   --check parses a finished package with exo_pkg_parse() and prints its
 *   header; build.sh runs it over every emitted package so a bad manifest
 *   fails the build rather than the boot.
 */

#include "../../src/exo_pkg.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The image's own footprint, in pages, plus slack for the heap a LibOS grows
 * on top of it. Rough on purpose: SCRUM-232 owns real admission numbers; this
 * only has to satisfy exo_pkg_check_header()'s min <= recommended. */
#define HEAP_SLACK_PAGES 16u

static unsigned char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        perror(path);
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
        perror(path);
        fclose(f);
        return NULL;
    }
    /* malloc(1) minimum so an empty file (a program with no .data) still
     * yields a non-NULL pointer to hand exo_pkg_pack(). */
    unsigned char *buf = malloc((size_t)n + 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "exopkg: could not read %s\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static int parse_want(const char *s, uint32_t *out)
{
    uint32_t want = 0;
    char tmp[64];
    /* An empty list or an empty element ("", "fb,,kbd", "fb,") is a typo or
     * an empty shell variable, not "no resources": say "none" for that. */
    if (s[0] == '\0' || s[0] == ',' || s[strlen(s) - 1] == ',' ||
        strstr(s, ",,") != NULL)
        return -1;
    if (strlen(s) >= sizeof(tmp))
        return -1;
    strcpy(tmp, s);
    for (char *tok = strtok(tmp, ","); tok; tok = strtok(NULL, ",")) {
        if (strcmp(tok, "fb") == 0)
            want |= EXO_PKG_WANT_FRAMEBUFFER;
        else if (strcmp(tok, "disk") == 0)
            want |= EXO_PKG_WANT_DISK;
        else if (strcmp(tok, "kbd") == 0)
            want |= EXO_PKG_WANT_KEYBOARD;
        else if (strcmp(tok, "wad") == 0)
            want |= EXO_PKG_WANT_WAD;
        else if (strcmp(tok, "progvol") == 0)
            want |= EXO_PKG_WANT_PROGRAM_RO;
        else if (strcmp(tok, "none") != 0)
            return -1;
    }
    *out = want;
    return 0;
}

static int parse_u32(const char *s, uint32_t *out)
{
    errno = 0;
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    if (*s == '\0' || *end != '\0' || errno == ERANGE || v > UINT32_MAX)
        return -1;
    *out = (uint32_t)v;
    return 0;
}

static int check_package(const char *path)
{
    size_t len;
    unsigned char *buf = read_file(path, &len);
    if (!buf)
        return 1;
    exo_pkg_header_t h;
    const void *code, *data;
    int rc = exo_pkg_parse(buf, len, &h, &code, &data);
    if (rc != 0) {
        fprintf(stderr, "exopkg: %s: exo_pkg_parse failed: %d\n", path, rc);
        return 1;
    }
    printf("    exopkg: %s ok: code %u data %u bss %u stack %u params %u "
           "pages %u/%u voices %u want 0x%x volume %u+%u/%u\n",
           path, h.code_len, h.data_len, h.bss_len, h.stack_pages,
           h.params_len, h.min_pages, h.recommended_pages, h.sound_voices,
           h.want_flags, h.volume_slot, exo_pkg_volume_slot_count(&h),
           h.volume_sectors);
    free(buf);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--check") == 0)
        return check_package(argv[2]);

    if (argc < 6 || (argc - 6) % 2 != 0) {
        fprintf(stderr,
                "usage: %s <out.exo> <code.bin> <data.bin> <bss-len> <want>"
                " [--params-len N] [--sound-voices N] [--min-pages N]"
                " [--recommended-pages N] [--stack-pages N]"
                " [--volume-slot N [--volume-slots N] --volume-sectors N]\n"
                "       %s --check <package.exo>\n",
                argv[0], argv[0]);
        return 1;
    }

    uint32_t params_len = 0, sound_voices = 0, stack_pages = EXO_PKG_MAX_STACK_PAGES;
    uint32_t min_pages = 0, rec_pages = 0;
    uint32_t volume_slot = 0, volume_slots = 0, volume_sectors = 0;
    int have_min = 0, have_rec = 0;
    for (int i = 6; i < argc; i += 2) {
        uint32_t *dst = NULL;
        if (strcmp(argv[i], "--params-len") == 0)
            dst = &params_len;
        else if (strcmp(argv[i], "--sound-voices") == 0)
            dst = &sound_voices;
        else if (strcmp(argv[i], "--volume-slot") == 0)
            dst = &volume_slot;
        else if (strcmp(argv[i], "--volume-slots") == 0)
            dst = &volume_slots;
        else if (strcmp(argv[i], "--volume-sectors") == 0)
            dst = &volume_sectors;
        else if (strcmp(argv[i], "--stack-pages") == 0)
            dst = &stack_pages;
        else if (strcmp(argv[i], "--min-pages") == 0) {
            dst = &min_pages;
            have_min = 1;
        } else if (strcmp(argv[i], "--recommended-pages") == 0) {
            dst = &rec_pages;
            have_rec = 1;
        }
        if (!dst || parse_u32(argv[i + 1], dst) != 0) {
            fprintf(stderr, "exopkg: bad option '%s %s'\n", argv[i], argv[i + 1]);
            return 1;
        }
    }

    errno = 0;
    char *end;
    unsigned long bss_len = strtoul(argv[4], &end, 10);
    if (*argv[4] == '\0' || *end != '\0' || errno == ERANGE ||
        bss_len > UINT32_MAX) {
        fprintf(stderr, "exopkg: invalid <bss-len> '%s'\n", argv[4]);
        return 1;
    }

    uint32_t want;
    if (parse_want(argv[5], &want) != 0) {
        fprintf(stderr, "exopkg: invalid <want> '%s' (fb,disk,kbd,wad,progvol or none)\n",
                argv[5]);
        return 1;
    }

    size_t code_len, data_len;
    unsigned char *code = read_file(argv[2], &code_len);
    unsigned char *data = read_file(argv[3], &data_len);
    if (!code || !data)
        return 1;

    exo_pkg_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic       = EXO_PKG_MAGIC;
    hdr.version     = EXO_PKG_VERSION;
    hdr.code_len    = (uint32_t)code_len;
    hdr.data_len    = (uint32_t)data_len;
    hdr.bss_len     = (uint32_t)bss_len;
    hdr.stack_pages = stack_pages;
    hdr.params_len  = params_len;
    hdr.min_pages   = have_min ? min_pages
                               : exo_pkg_footprint_pages((uint32_t)code_len,
                                                         (uint32_t)data_len,
                                                         (uint32_t)bss_len,
                                                         want);
    hdr.recommended_pages = have_rec ? rec_pages
                                     : hdr.min_pages + HEAP_SLACK_PAGES;
    hdr.sound_voices = sound_voices;
    hdr.want_flags   = want;
    hdr.volume_slot    = volume_slot;
    hdr.volume_sectors = volume_sectors;
    hdr.volume_slots   = volume_slots;

    size_t cap = sizeof(hdr) + code_len + data_len;
    unsigned char *out = malloc(cap);
    if (!out) {
        fprintf(stderr, "exopkg: out of memory\n");
        return 1;
    }

    size_t total;
    int rc = exo_pkg_pack(out, cap, &hdr, code, data, &total);
    if (rc != 0) {
        fprintf(stderr, "exopkg: exo_pkg_pack failed: %d\n", rc);
        return 1;
    }

    FILE *f = fopen(argv[1], "wb");
    if (!f || fwrite(out, 1, total, f) != total || fclose(f) != 0) {
        fprintf(stderr, "exopkg: could not write %s\n", argv[1]);
        return 1;
    }

    printf("    exopkg: %s: %zu bytes (code %zu, data %zu, bss %lu)\n",
           argv[1], total, code_len, data_len, bss_len);
    return 0;
}
