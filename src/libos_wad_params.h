#ifndef LIBOS_WAD_PARAMS_H
#define LIBOS_WAD_PARAMS_H

#include <stdint.h>

/*
 * libos_wad_params_t — the one thing kernel_main tells the ring-3 WAD/
 * automap viewer (src/libos_wad_viewer/libos_wad_viewer.c) that it has no
 * other way to learn: where libos_map_wad() (src/libos_wad_map.h) put the
 * WAD and how big it is. Everything else the viewer needs (the framebuffer,
 * the keyboard) it gets itself through ordinary syscalls.
 *
 * SCRUM-175: this struct is the viewer's own g_wad_params global -- the
 * first global declared in libos_wad_viewer.c, so it lands at offset 0 of
 * that LibOS's compiled .data section -- and src/syscall_launch.c patches it
 * in place after libos_build_image() succeeds via
 * libos_launch_patch_params()/img.data_paddrs[0] (src/libos_launch.h), the
 * generalized alternative to a per-app side-channel params page. (An earlier
 * version of this mechanism instead hand-mapped a second page at its own
 * fixed VA, LIBOS_WAD_PARAMS_VADDR -- removed; see that helper's own
 * comment for why the .data-resident convention replaces it.) Small enough
 * that ABI layout care (explicit padding, a _Static_assert'd size) buys
 * nothing here: both sides are compiled from this same header in the same
 * build, unlike exo_syscall.h's structs which cross a `syscall` boundary
 * between independently-evolvable kernel and LibOS binaries.
 */
typedef struct {
    uint64_t wad_vaddr;  /* LIBOS_WAD_VADDR -- restated here so the viewer
                          * does not need to link libos_wad_map.h's kernel-
                          * only header just to know its own base address */
    uint64_t wad_size;   /* exact byte length wad_init() should validate */
} libos_wad_params_t;

/*
 * The one name three independent places have to agree on (SCRUM-190):
 *
 *   - docker/scripts/build.sh's host-side image builder writes the WAD
 *     under this name onto build/exodoom_disk.img's ExoFS volume;
 *   - src/syscall_launch.c's LAUNCH_NEEDS_WAD path opens this same name
 *     (absolute path "/" + this) when mounting that volume at launch time;
 *   - src/doomgeneric_exo.c's DOOM_IWAD_BLOB_NAME is this value, because
 *     Doom's D_FindIWAD() -> fopen() has to find the blob DG_Init registers
 *     under the name it actually searches for.
 *
 * One #define here rather than three independent string literals is what
 * build.sh's own build-time consistency check (step [1b/7]) can verify
 * against -- see that script's "IWAD filename is spelled in two places"
 * comment for the drift this is guarding against (and why it used to be
 * three before the GRUB multiboot module went away).
 */
#define LIBOS_WAD_FILE_NAME "freedoom2.wad"

#endif /* LIBOS_WAD_PARAMS_H */
