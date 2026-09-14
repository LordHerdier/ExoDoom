#include "libos_fb.h"
#include "exo_syscall.h"

/*
 * Two build targets, same file — the same split libos_page_alloc.c uses
 * (see that file's own top comment for the full reasoning): compiled with
 * -DEXO_KERNEL (every ordinary kernel build, including this file's own
 * kernel-context unit tests), the exo_syscall.h `syscall`-instruction stubs
 * do not even exist, so the three operations below go through
 * exo_syscall_dispatch() directly — a plain C call, no ring transition.
 * Compiled WITHOUT it (the ring-3 libc-shim link target, SCRUM-51), they go
 * through the real stubs and take a genuine ring 3 -> ring 0 -> ring 3 round
 * trip.
 */
#ifdef EXO_KERNEL
#include "syscall.h"
#endif
#include <stddef.h>

/*
 * Fixed LibOS-window address for the mapped framebuffer, chosen clear of
 * every other fixed/scratch window this codebase already hands out inside
 * [EXO_USER_VA_BASE, EXO_USER_VA_END):
 *   - libos_launch.h's code/data/stack triple sits below
 *     EXO_USER_VA_BASE + 0x20000.
 *   - src/libos_page_alloc.c's heap window is
 *     [EXO_USER_VA_BASE + 0x1000000, EXO_USER_VA_BASE + 0x2000000).
 * 64 MiB in leaves a wide, untouched gap ahead of both, with room to spare
 * below the next fixed window any future subsystem picks. */
#define LIBOS_FB_VADDR (EXO_USER_VA_BASE + 0x4000000ULL)

#define LIBOS_FB_PAGE_SIZE 4096ULL

#ifdef EXO_KERNEL

static int64_t do_fb_acquire(exo_fb_info_t *info_out)
{
    return exo_syscall_dispatch(EXO_SYS_FB_ACQUIRE,
                                (uint64_t)(uintptr_t)info_out,
                                0, 0, 0, 0, 0);
}

static int64_t do_page_map(uint64_t vaddr, uint64_t paddr, uint64_t flags)
{
    return exo_syscall_dispatch(EXO_SYS_PAGE_MAP, vaddr, paddr, flags,
                                0, 0, 0);
}

static int64_t do_page_unmap(uint64_t vaddr)
{
    return exo_syscall_dispatch(EXO_SYS_PAGE_UNMAP, vaddr, 0, 0, 0, 0, 0);
}

#else /* !EXO_KERNEL — the ring-3 LibOS link target (SCRUM-51) */

static int64_t do_fb_acquire(exo_fb_info_t *info_out)
{
    return exo_fb_acquire(info_out);
}

static int64_t do_page_map(uint64_t vaddr, uint64_t paddr, uint64_t flags)
{
    return exo_page_map(vaddr, paddr, (uint32_t)flags);
}

static int64_t do_page_unmap(uint64_t vaddr)
{
    return exo_page_unmap(vaddr);
}

#endif /* EXO_KERNEL */

int libos_fb_map(libos_fb_t *fb_out)
{
    if (fb_out == NULL) {
        return -EXO_EFAULT;
    }

    exo_fb_info_t info;
    int64_t rc = do_fb_acquire(&info);
    if (rc < 0) {
        return (int)rc;
    }

    uint64_t fb_bytes = (uint64_t)info.pitch * (uint64_t)info.height;
    uint64_t pages = (fb_bytes + LIBOS_FB_PAGE_SIZE - 1) / LIBOS_FB_PAGE_SIZE;

    for (uint64_t i = 0; i < pages; i++) {
        uint64_t vaddr = LIBOS_FB_VADDR + i * LIBOS_FB_PAGE_SIZE;
        uint64_t paddr = info.phys_addr + i * LIBOS_FB_PAGE_SIZE;

        int64_t map_rc = do_page_map(vaddr, paddr,
                                     EXO_PAGE_WRITE | EXO_PAGE_USER);
        if (map_rc < 0) {
            for (uint64_t j = 0; j < i; j++) {
                do_page_unmap(LIBOS_FB_VADDR + j * LIBOS_FB_PAGE_SIZE);
            }
            return (int)map_rc;
        }
    }

    fb_out->vaddr  = (void *)(uintptr_t)LIBOS_FB_VADDR;
    fb_out->width  = info.width;
    fb_out->height = info.height;
    fb_out->pitch  = info.pitch;
    fb_out->bpp    = info.bpp;
    return 0;
}
