#pragma once
#include <stdint.h>

/*
 * exo_syscall.h — ExoDoom exokernel syscall ABI (SCRUM-24).
 *
 * The C expression of docs/syscall_spec.md §3.  That document is the source
 * of truth: syscall numbers here match §3.2 and the register convention
 * encoded in the stubs below matches §3.1.  Change one, change the other.
 *
 * Both sides include this header:
 *
 *   - The kernel builds with EXO_KERNEL defined and gets only the numbers, the
 *     shared argument structs, and the error codes — it must not see the
 *     user-side stubs, which would issue a `syscall` against itself.  The
 *     define comes from -DEXO_KERNEL on the kernel compiler command line in
 *     docker/scripts/build.sh, not from a #define in each file: with #pragma
 *     once, a #define placed after any transitive include of this header would
 *     be too late and would silently lock in the LibOS view.
 *   - The LibOS includes it plainly and additionally gets the inline stubs
 *     (exo_syscall0..6) plus one typed wrapper per syscall.
 *
 * Calling convention (x86_64 `syscall`, docs/syscall_spec.md §3.1):
 *
 *   number  RAX
 *   args    RDI, RSI, RDX, R10, R8, R9   (R10, not RCX — see below)
 *   return  RAX, negative values are -EXO_E* error codes
 *
 * `syscall` itself overwrites RCX with the return RIP and R11 with RFLAGS,
 * which is why the 4th argument travels in R10 rather than in the RCX the
 * SysV C ABI would use, and why every stub clobbers both.
 *
 * The kernel must preserve every other register, argument registers included.
 * That is the Linux guarantee and it is stronger than the SysV C ABI, under
 * which RDI/RSI/RDX/R10/R8/R9 are caller-saved.  The stubs below depend on it:
 * they pass arguments as plain inputs rather than clobbers, so the compiler may
 * keep a value live in an argument register across the `syscall` and reuse it
 * on the next call.  A dispatcher (SCRUM-32) that restores only the
 * callee-saved set would corrupt arguments in any loop that repeats a syscall.
 * See docs/syscall_spec.md §3.1.
 *
 * Nothing here is implemented yet — the dispatch side is SCRUM-32 and the
 * individual handlers land across Sprints 3-5.  The header exists so the
 * kernel dispatcher and the LibOS libc shim (SCRUM-51) agree on the ABI
 * before either is written.
 */

/* ---- Syscall numbers (docs/syscall_spec.md §3.2) ------------------------ */

/* Memory */
#define EXO_SYS_PAGE_ALLOC    0
#define EXO_SYS_PAGE_FREE     1
#define EXO_SYS_PAGE_MAP      2
#define EXO_SYS_PAGE_UNMAP    3
/* Framebuffer */
#define EXO_SYS_FB_ACQUIRE    4
/* Timer */
#define EXO_SYS_GET_TICKS     5
/* Input */
#define EXO_SYS_KBD_POLL      6
#define EXO_SYS_MOUSE_POLL    7
/* Debug */
#define EXO_SYS_SERIAL_WRITE  8
/* File I/O */
#define EXO_SYS_FILE_OPEN     9
#define EXO_SYS_FILE_CLOSE   10
#define EXO_SYS_FILE_READ    11
#define EXO_SYS_FILE_WRITE   12
#define EXO_SYS_FILE_SEEK    13
#define EXO_SYS_FILE_STAT    14
#define EXO_SYS_FILE_REMOVE  15
#define EXO_SYS_FILE_RENAME  16
/* Sound */
#define EXO_SYS_SOUND_TONE   17
#define EXO_SYS_SOUND_STOP   18
/* Scheduling / lifecycle */
#define EXO_SYS_YIELD        19
#define EXO_SYS_EXIT         20

/* One past the highest valid number.  The dispatcher rejects anything >= this
 * with -EXO_ENOSYS; keep it last and keep the numbers above dense. */
#define EXO_SYS_COUNT        21

/* ---- Error codes -------------------------------------------------------- */
/*
 * Returned negated in RAX: a syscall that fails with EXO_ENOMEM returns
 * -EXO_ENOMEM.  Values match the Linux errno numbers of the same names so a
 * later libc errno.h can pass them through unmodified.  Prefixed because the
 * libc shim will define the unprefixed names for Doom (Sprint 3).
 */
#define EXO_EPERM     1   /* operation not permitted for this LibOS       */
#define EXO_ENOENT    2   /* no such file                                 */
#define EXO_EBADF     9   /* bad file descriptor                          */
#define EXO_ENOMEM   12   /* out of physical pages / heap                 */
#define EXO_EACCES   13   /* permission denied                            */
#define EXO_EFAULT   14   /* pointer argument outside caller address space */
#define EXO_EBUSY    16   /* resource held by another LibOS (framebuffer) */
#define EXO_EINVAL   22   /* malformed or out-of-range argument           */
#define EXO_EMFILE   24   /* file descriptor table full                   */
#define EXO_ENOSPC   28   /* ramdisk full                                 */
#define EXO_ENOSYS   38   /* syscall number not implemented               */

/* ---- Argument constants ------------------------------------------------- */

/* exo_page_map flags.  Present is implied by the mapping itself. */
#define EXO_PAGE_READ   (1u << 0)
#define EXO_PAGE_WRITE  (1u << 1)
#define EXO_PAGE_USER   (1u << 2)

/* exo_file_open modes (docs/syscall_spec.md §3.2 #9) */
#define EXO_O_RDONLY    0
#define EXO_O_WRONLY    1
#define EXO_O_RDWR      2

/* exo_file_seek whence values, matching stdio's SEEK_* ordering (#13) */
#define EXO_SEEK_SET    0
#define EXO_SEEK_CUR    1
#define EXO_SEEK_END    2

/* ---- Shared argument structs -------------------------------------------- */
/*
 * These cross the kernel/LibOS boundary, so their layout is ABI.  Explicit
 * reserved fields keep the size fixed and the members naturally aligned;
 * everything is little-endian x86_64 and the sizes are asserted below.
 */

/* exo_fb_acquire(info_out) — #4 */
typedef struct {
    uint64_t phys_addr;   /* framebuffer base, physical; LibOS maps it itself */
    uint32_t width;       /* pixels                                           */
    uint32_t height;      /* pixels                                           */
    uint32_t pitch;       /* bytes per scanline, may exceed width * bpp / 8   */
    uint8_t  bpp;         /* bits per pixel; 32 (BGRX8888) on QEMU today      */
    uint8_t  reserved[3]; /* zeroed by the kernel                             */
} exo_fb_info_t;

/* Modifier bits in exo_kbd_event_t.modifiers.  Sampled when the event was
 * queued, so a chord reads correctly even if the modifier is released before
 * the LibOS polls.  Values mirror the kernel's MOD_* in src/ps2.h; they are
 * restated here because ps2.h is kernel-only and the LibOS cannot include it. */
#define EXO_MOD_LSHIFT  (1u << 0)
#define EXO_MOD_RSHIFT  (1u << 1)
#define EXO_MOD_LCTRL   (1u << 2)
#define EXO_MOD_RCTRL   (1u << 3)
#define EXO_MOD_LALT    (1u << 4)
#define EXO_MOD_RALT    (1u << 5)

/* Either key of a pair — Doom binds shift/ctrl/alt without caring which side. */
#define EXO_MOD_SHIFT   (EXO_MOD_LSHIFT | EXO_MOD_RSHIFT)
#define EXO_MOD_CTRL    (EXO_MOD_LCTRL  | EXO_MOD_RCTRL)
#define EXO_MOD_ALT     (EXO_MOD_LALT   | EXO_MOD_RALT)

/* exo_kbd_poll(event_out) — #6.
 *
 * `key` is a decoded ps2_key_t index (KEY_A, KEY_ESC, ...), not a raw PS/2
 * set-1 scancode: the kernel's scancode decoder runs before the event is
 * queued, so the LibOS never sees the wire bytes or the 0xE0 prefixes.  What
 * stays LibOS policy is the ps2_key_t -> Doom keycode mapping, which lives in
 * the Doom port (Sprint 4).
 *
 * The layout matches the kernel's kbd_event_t (src/ps2.h) field for field —
 * `key` is that struct's `key` — but the two are not the same type: this one
 * carries a trailing reserved byte and is ABI, while the kernel struct is free
 * to grow.  The #6 handler converts field by field rather than casting. */
typedef struct {
    uint8_t pressed;      /* 1 = key down, 0 = key up            */
    uint8_t key;          /* decoded ps2_key_t index             */
    uint8_t modifiers;    /* EXO_MOD_* mask held when queued     */
    uint8_t reserved;     /* zeroed by the kernel                */
} exo_kbd_event_t;

/* exo_mouse_poll(state_out) — #7.  Deltas accumulate in the kernel between
 * calls and are reset to zero by each poll; buttons are a level, not a delta. */
typedef struct {
    int16_t dx;
    int16_t dy;
    uint8_t buttons;      /* bit 0 left, bit 1 right, bit 2 middle */
    uint8_t reserved;
} exo_mouse_state_t;

/* Layout is ABI — break it and the kernel and LibOS silently disagree. */
_Static_assert(sizeof(exo_fb_info_t)     == 24, "exo_fb_info_t layout is ABI");
_Static_assert(sizeof(exo_kbd_event_t)   ==  4, "exo_kbd_event_t layout is ABI");
_Static_assert(sizeof(exo_mouse_state_t) ==  6, "exo_mouse_state_t layout is ABI");

#ifndef EXO_KERNEL

/* ---- Raw syscall stubs (LibOS side only) -------------------------------- */
/*
 * One stub per argument count.  Arguments 1-3 use the "D", "S", "d"
 * constraints (RDI, RSI, RDX); arguments 4-6 have no constraint letter, so
 * they go through explicit register variables for R10, R8 and R9.
 *
 * "memory" is in every clobber list because the kernel may read or write
 * through pointer arguments, which the compiler cannot see.
 */

static inline int64_t exo_syscall0(uint64_t num)
{
    int64_t ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(num)
                     : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t exo_syscall1(uint64_t num, uint64_t a1)
{
    int64_t ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(num), "D"(a1)
                     : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t exo_syscall2(uint64_t num, uint64_t a1, uint64_t a2)
{
    int64_t ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(num), "D"(a1), "S"(a2)
                     : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t exo_syscall3(uint64_t num, uint64_t a1, uint64_t a2,
                                   uint64_t a3)
{
    int64_t ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(num), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t exo_syscall4(uint64_t num, uint64_t a1, uint64_t a2,
                                   uint64_t a3, uint64_t a4)
{
    int64_t ret;
    register uint64_t r10 __asm__("r10") = a4;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
                     : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t exo_syscall5(uint64_t num, uint64_t a1, uint64_t a2,
                                   uint64_t a3, uint64_t a4, uint64_t a5)
{
    int64_t ret;
    register uint64_t r10 __asm__("r10") = a4;
    register uint64_t r8  __asm__("r8")  = a5;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t exo_syscall6(uint64_t num, uint64_t a1, uint64_t a2,
                                   uint64_t a3, uint64_t a4, uint64_t a5,
                                   uint64_t a6)
{
    int64_t ret;
    register uint64_t r10 __asm__("r10") = a4;
    register uint64_t r8  __asm__("r8")  = a5;
    register uint64_t r9  __asm__("r9")  = a6;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8),
                       "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
}

/* ---- Typed wrappers ----------------------------------------------------- */
/*
 * One per syscall, named and ordered as in §3.2.  Return values are the raw
 * RAX result: >= 0 on success, -EXO_E* on failure.  Pointer arguments are
 * LibOS virtual addresses; the kernel validates them (Sprint 6) and returns
 * -EXO_EFAULT rather than faulting.
 */

/* #0 — allocate one 4K physical page.  Returns its physical address (which is
 * non-zero: page 0 is never handed out) or -EXO_ENOMEM. */
static inline int64_t exo_page_alloc(void)
{
    return exo_syscall0(EXO_SYS_PAGE_ALLOC);
}

/* #1 — release a page from exo_page_alloc.  0, or -EXO_EINVAL if paddr was
 * never allocated to this LibOS. */
static inline int64_t exo_page_free(uint64_t paddr)
{
    return exo_syscall1(EXO_SYS_PAGE_FREE, paddr);
}

/* #2 — map paddr at vaddr in the caller's address space.  flags is a mask of
 * EXO_PAGE_*.  0, -EXO_EINVAL or -EXO_EFAULT. */
static inline int64_t exo_page_map(uint64_t vaddr, uint64_t paddr,
                                   uint32_t flags)
{
    return exo_syscall3(EXO_SYS_PAGE_MAP, vaddr, paddr, (uint64_t)flags);
}

/* #3 — remove the mapping at vaddr.  0 or -EXO_EINVAL.  Does not free the
 * underlying page; call exo_page_free for that. */
static inline int64_t exo_page_unmap(uint64_t vaddr)
{
    return exo_syscall1(EXO_SYS_PAGE_UNMAP, vaddr);
}

/* #4 — claim the framebuffer and describe it.  The LibOS maps the reported
 * physical range itself with exo_page_map.  0, or -EXO_EBUSY if another LibOS
 * holds it.  Used by DG_Init. */
static inline int64_t exo_fb_acquire(exo_fb_info_t *info_out)
{
    return exo_syscall1(EXO_SYS_FB_ACQUIRE, (uint64_t)(uintptr_t)info_out);
}

/* #5 — milliseconds since boot, monotonic.  Never fails.  Backs both
 * DG_GetTicksMs and DG_SleepMs. */
static inline int64_t exo_get_ticks(void)
{
    return exo_syscall0(EXO_SYS_GET_TICKS);
}

/* #6 — dequeue one keyboard event.  1 if *event_out was filled, 0 if the ring
 * was empty.  Non-blocking.  Backs DG_GetKey. */
static inline int64_t exo_kbd_poll(exo_kbd_event_t *event_out)
{
    return exo_syscall1(EXO_SYS_KBD_POLL, (uint64_t)(uintptr_t)event_out);
}

/* #7 — read and reset the accumulated mouse deltas.  Always 0. */
static inline int64_t exo_mouse_poll(exo_mouse_state_t *state_out)
{
    return exo_syscall1(EXO_SYS_MOUSE_POLL, (uint64_t)(uintptr_t)state_out);
}

/* #8 — write len bytes of buf to COM1.  Returns bytes written or -EXO_EFAULT.
 * Backs the printf / fprintf shim. */
static inline int64_t exo_serial_write(const void *buf, uint64_t len)
{
    return exo_syscall2(EXO_SYS_SERIAL_WRITE, (uint64_t)(uintptr_t)buf, len);
}

/* #9 — open path on the ramdisk.  mode is one of EXO_O_*.  Returns an fd >= 0,
 * or -EXO_ENOENT / -EXO_EMFILE.  Backs fopen. */
static inline int64_t exo_file_open(const char *path, uint32_t mode)
{
    return exo_syscall2(EXO_SYS_FILE_OPEN, (uint64_t)(uintptr_t)path,
                        (uint64_t)mode);
}

/* #10 — close fd.  0 or -EXO_EBADF. */
static inline int64_t exo_file_close(int32_t fd)
{
    return exo_syscall1(EXO_SYS_FILE_CLOSE, (uint64_t)(int64_t)fd);
}

/* #11 — read up to count bytes into buf.  Bytes read, 0 at EOF, or negative
 * error.  Backs fread. */
static inline int64_t exo_file_read(int32_t fd, void *buf, uint64_t count)
{
    return exo_syscall3(EXO_SYS_FILE_READ, (uint64_t)(int64_t)fd,
                        (uint64_t)(uintptr_t)buf, count);
}

/* #12 — write count bytes from buf.  Bytes written or negative error.  Backs
 * fwrite. */
static inline int64_t exo_file_write(int32_t fd, const void *buf,
                                     uint64_t count)
{
    return exo_syscall3(EXO_SYS_FILE_WRITE, (uint64_t)(int64_t)fd,
                        (uint64_t)(uintptr_t)buf, count);
}

/* #13 — seek.  whence is one of EXO_SEEK_*.  Returns the new absolute
 * position or negative error.  Backs fseek and ftell. */
static inline int64_t exo_file_seek(int32_t fd, int64_t offset, uint32_t whence)
{
    return exo_syscall3(EXO_SYS_FILE_SEEK, (uint64_t)(int64_t)fd,
                        (uint64_t)offset, (uint64_t)whence);
}

/* #14 — write the size of path to *size_out.  0 or -EXO_ENOENT.  Backs
 * M_FileExists and M_FileLength. */
static inline int64_t exo_file_stat(const char *path, uint64_t *size_out)
{
    return exo_syscall2(EXO_SYS_FILE_STAT, (uint64_t)(uintptr_t)path,
                        (uint64_t)(uintptr_t)size_out);
}

/* #15 — delete path.  0 or -EXO_ENOENT.  Backs remove(). */
static inline int64_t exo_file_remove(const char *path)
{
    return exo_syscall1(EXO_SYS_FILE_REMOVE, (uint64_t)(uintptr_t)path);
}

/* #16 — rename oldpath to newpath.  0 or negative error.  Backs rename(),
 * used for save game rotation. */
static inline int64_t exo_file_rename(const char *oldpath, const char *newpath)
{
    return exo_syscall2(EXO_SYS_FILE_RENAME, (uint64_t)(uintptr_t)oldpath,
                        (uint64_t)(uintptr_t)newpath);
}

/* #17 — start a PC speaker tone.  Returns immediately; the kernel stops the
 * tone after dur_ms.  Always 0. */
static inline int64_t exo_sound_tone(uint32_t freq, uint32_t dur_ms)
{
    return exo_syscall2(EXO_SYS_SOUND_TONE, (uint64_t)freq, (uint64_t)dur_ms);
}

/* #18 — silence the speaker now.  Always 0. */
static inline int64_t exo_sound_stop(void)
{
    return exo_syscall0(EXO_SYS_SOUND_STOP);
}

/* #19 — yield to the next runnable LibOS.  Returns 0 when rescheduled. */
static inline int64_t exo_yield(void)
{
    return exo_syscall0(EXO_SYS_YIELD);
}

/* #20 — terminate this LibOS.  Does not return; the loop only silences
 * "function returns" diagnostics if the kernel ever misbehaves. */
static inline void exo_exit(int32_t code)
{
    exo_syscall1(EXO_SYS_EXIT, (uint64_t)(int64_t)code);
    for (;;) { }
}

#endif /* !EXO_KERNEL */
