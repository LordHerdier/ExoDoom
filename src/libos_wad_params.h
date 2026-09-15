#ifndef LIBOS_WAD_PARAMS_H
#define LIBOS_WAD_PARAMS_H

#include <stdint.h>

/*
 * libos_wad_params_t — the one thing kernel_main tells the ring-3 WAD/
 * automap viewer (src/libos_wad_viewer.c) that it has no other way to learn:
 * where libos_map_wad() (src/libos_wad_map.h) put the WAD and how big it is.
 * Everything else the viewer needs (the framebuffer, the keyboard) it gets
 * itself through ordinary syscalls.
 *
 * This is libos_build_image()'s `data` blob verbatim -- kernel_main builds
 * one of these on its stack and passes it as `data`/`data_len`, and the
 * viewer reads it back through the fixed immediate LIBOS_LAUNCH_DATA_VADDR
 * (src/libos_launch.h), the same convention every ring-3 entry point uses
 * for its own data (see that header's top comment). Small enough that ABI
 * layout care (explicit padding, a _Static_assert'd size) buys nothing here:
 * both sides are compiled from this same header in the same build, unlike
 * exo_syscall.h's structs which cross a `syscall` boundary between
 * independently-evolvable kernel and LibOS binaries.
 */
typedef struct {
    uint64_t wad_vaddr;  /* LIBOS_WAD_VADDR -- restated here so the viewer
                          * does not need to link libos_wad_map.h's kernel-
                          * only header just to know its own base address */
    uint64_t wad_size;   /* exact byte length wad_init() should validate */
} libos_wad_params_t;

/*
 * Fixed LibOS-window address for this one-page params struct -- deliberately
 * NOT LIBOS_LAUNCH_DATA_VADDR. libos_build_image()'s `data` argument is the
 * ring-3 target's own compiled .data section (src/libos_wad_viewer.c links a
 * real mutable global there -- see its own top comment on why one has to
 * exist at all), and overwriting that region with this struct instead, as an
 * earlier version of this mechanism did, would silently corrupt it. One page
 * below LIBOS_WAD_VADDR, kernel_main maps this itself with a plain
 * vmm_map_page_in() call (no libos_wad_map.h needed on this, the LibOS-
 * visible side) right after libos_build_image() succeeds. */
#define LIBOS_WAD_PARAMS_VADDR (0x0000400000000000ULL + 0x7000000ULL)

#endif /* LIBOS_WAD_PARAMS_H */
