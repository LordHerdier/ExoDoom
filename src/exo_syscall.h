#pragma once

#ifndef __ASSEMBLER__
#include <stdint.h>
#endif

#include "exo_errno.h"

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
 *     shared argument structs, and the error codes (exo_errno.h) — it must
 *     not see the user-side stubs, which would issue a `syscall` against
 *     itself.  The
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
 *
 * A third audience as of SCRUM-50: bare assembly test probes (the .s files
 * under tests/kernel) that need a syscall number or a LibOS-window constant
 * without hardcoding it a second time. docker/scripts/build.sh preprocesses
 * those files with the C preprocessor (`-x assembler-with-cpp`, which
 * predefines `__ASSEMBLER__`) before handing them to `as`, so every typedef,
 * struct and function prototype below that an assembler cannot parse is
 * wrapped in `#ifndef __ASSEMBLER__` — only the #define numbers survive into
 * assembly text. Keep that wrapping intact when adding new C-only content
 * here.
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
/* File I/O -- reserved, never bound. Files are LibOS-side policy over
 * exo_disk_read/exo_disk_write: exo_file_* is a library in
 * src/libos_fs/exo_file.h (SCRUM-44), not a syscall. The numbers stay
 * reserved rather than reused so an old caller gets -EXO_ENOSYS, and #9 is
 * what test_syscall_k.c / test_syscall_bench_k.c use as "an unbound
 * number". */
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
/* LibOS launch (SCRUM-178, SCRUM-168, SCRUM-182, SCRUM-66, SCRUM-184).
 *
 * ONE number for every launchable app, taking an EXO_LAUNCH_APP_* id below.
 * It replaced four -- EXO_SYS_LAUNCH_WAD_VIEWER/_CLOCK/_SNAKE/_DOOM, #21..#24
 * -- which did not scale: each new demo LibOS cost a syscall number, a
 * dispatcher handler, and edits to the two tests that police this table's
 * density and size, to say nothing of the number space itself. The app id is
 * an ordinary argument, so adding an app now touches neither. */
#define EXO_SYS_LAUNCH       21
/* Introspection (SCRUM-113) */
#define EXO_SYS_MEMSTAT      22
#define EXO_SYS_PSLIST       23
/* Disk (SCRUM-103, binding SCRUM-188) */
#define EXO_SYS_DISK_READ    24
#define EXO_SYS_DISK_WRITE   25
#define EXO_SYS_DISK_ACQUIRE 26
#define EXO_SYS_DISK_RELEASE 27
/* PCM sound (SCRUM-213).
 *
 * Appended rather than grouped next to #17/#18: the numbers above are ABI
 * for as long as this kernel image is the only thing that speaks it, and
 * renumbering the whole table to keep one family contiguous buys nothing
 * (SCRUM-184 renumbered only because collapsing four launch numbers left a
 * hole the density test rejects). There is no exo_sound_acquire to go with
 * these -- the mixer is shared, deliberately; see docs/syscall_spec.md
 * §3.8. */
#define EXO_SYS_SOUND_PCM      28
#define EXO_SYS_SOUND_PCM_STOP 29
/* #30 (SCRUM-214) re-places a voice already sounding. It exists because Doom
 * retunes live sounds -- I_UpdateSoundParams runs once a tic for every active
 * channel -- so a sound fired to the player's left has to follow them as they
 * turn. Same per-voice ownership as #29. */
#define EXO_SYS_SOUND_PCM_PARAMS 30
/* Disk capacity (SCRUM-202) -- unscoped introspection like MEMSTAT/PSLIST,
 * appended rather than grouped with #24-27 for the same reason #28-30 were
 * appended above: the numbers below are ABI for as long as this kernel image
 * is the only thing that speaks it. */
#define EXO_SYS_DISK_STAT     31
/* Streaming music (SCRUM-219).  Appended for the reason #28-30 were.  Unlike
 * the sfx calls these ARE bound: the music voice is one resource with one
 * holder, first writer wins, docs/syscall_spec.md §3.5b. */
#define EXO_SYS_SOUND_STREAM_WRITE  32
#define EXO_SYS_SOUND_STREAM_STOP   33
#define EXO_SYS_SOUND_STREAM_VOLUME 34
/* Wall-clock time (SCRUM-249).  Unscoped like #5: time is not a resource
 * anyone acquires.  Appended for the reason #28-30 were. */
#define EXO_SYS_RTC_READ            35
/* Launch from a package in the caller's memory (SCRUM-230).  Appended for the
 * reason #28-30 were.  #21 names a program the kernel image already contains;
 * this one takes the bytes of an exo_pkg.h package a ring-3 launcher read off
 * disk, so the set of runnable programs is no longer a link-time constant. */
#define EXO_SYS_LAUNCH_PKG          36
/* Which sectors of the disk are this program's own volume (SCRUM-237).
 * Appended for the reason #28-30 were. */
#define EXO_SYS_DISK_EXTENT         37
/* Free-resource report for a launcher's admission check (SCRUM-232).
 * Appended for the reason #28-30 were. */
#define EXO_SYS_RESSTAT             38
/* What one context was granted (SCRUM-235), and the second half of a held
 * launch (#36 with EXO_LAUNCH_HOLD).  Appended for the reason #28-30 were. */
#define EXO_SYS_GRANTSTAT           39
#define EXO_SYS_LAUNCH_START        40

/* One past the highest valid number.  The dispatcher rejects anything >= this
 * with -EXO_ENOSYS; keep it last and keep the numbers above dense.
 *
 * Collapsing the four launch numbers into one (SCRUM-184) left #22..#24
 * unclaimed, so MEMSTAT/PSLIST/DISK_* moved down three to close the gap
 * rather than leaving holes the density test would reject. Nothing outside
 * this kernel image depends on these values -- there is no stable userspace
 * ABI yet -- so renumbering is cheaper than a permanently sparse table. */
#define EXO_SYS_COUNT        41

/* ---- LibOS app ids, the argument to EXO_SYS_LAUNCH ---------------------- */
/*
 * These cross the syscall boundary, so the order is ABI: append, never
 * reorder, and keep EXO_LAUNCH_APP_COUNT one past the last. The kernel's
 * launch_apps[] table (src/syscall_launch.c) is indexed by exactly these
 * values and rejects anything >= COUNT with -EXO_EINVAL.
 *
 * Numbered from 0 rather than carrying over the old #21..#24 so the id is
 * an index rather than a syscall number wearing a disguise.
 */
#define EXO_LAUNCH_APP_WAD_VIEWER 0
#define EXO_LAUNCH_APP_DOOM       1
#define EXO_LAUNCH_APP_COUNT      2

/* Device indices exo_diskstat (#31) will answer for: [0, this). Device 0 is
 * reserved for the system disk and stays absent on a machine without one;
 * other disks are numbered from 1 (SCRUM-244). Enumerate by asking every
 * index, not by stopping at the first absent one. */
#define EXO_DISK_MAX_DEVICES      4u

/* Largest package exo_launch_pkg() will look at: the format's own worst case
 * (header + 192 code pages + 192 data pages; bss is not stored), so nothing
 * the loader could build is refused here. */
#define EXO_LAUNCH_PKG_MAX_LEN    (60u + 2u * 192u * 0x1000u)

/* ---- Error codes -------------------------------------------------------- */
/*
 * Moved to exo_errno.h (SCRUM-57) — included above, alongside <stdint.h>, so
 * every existing user of exo_syscall.h keeps seeing EXO_E* without change.
 */

/* ---- Argument constants ------------------------------------------------- */

/* exo_page_map flags.  Present is implied by the mapping itself.
 *
 * EXO_PAGE_READ is not representable on x86 — a present page is always
 * readable — so the dispatcher accepts it and ignores it rather than treating
 * its absence as -EXO_EINVAL.  EXO_PAGE_EXEC is defined now, while nothing
 * consumes the flag word yet, so that data, heap and framebuffer mappings can
 * be made non-executable once EFER.NXE is enabled; introducing it after the
 * ABI ships would mean renumbering the bits or bolting on a compat flag. */
#define EXO_PAGE_READ   (1u << 0)
#define EXO_PAGE_WRITE  (1u << 1)
#define EXO_PAGE_USER   (1u << 2)
#define EXO_PAGE_EXEC   (1u << 3)

/*
 * The virtual address window a LibOS may map into (docs/syscall_spec.md §3.7).
 *
 * exo_page_map / exo_page_unmap accept a `vaddr` in [BASE, END) and answer
 * -EXO_EPERM anywhere else.  The window exists because the kernel and the
 * LibOS share one address space until SCRUM-48: a mapping syscall edits the
 * page tables the kernel runs on, so ownership of a *physical* page must not
 * become a licence to install it over kernel text.
 *
 * The base is 64 TiB, and the reason it is not lower is worth stating plainly,
 * because the obvious choice is wrong: the kernel map is an *identity* map
 * (vmm_init, SCRUM-15), so every byte of usable RAM is mapped at a virtual
 * address equal to its physical one.  A window that starts below the top of
 * physical memory therefore overlaps real kernel mappings — on a machine with
 * 8 GiB of RAM, a window at 4 GiB lands squarely inside mapped memory, where
 * exo_page_map can map nothing (the address is already taken) and, worse,
 * exo_page_unmap would happily unmap the kernel's own RAM.  64 TiB is chosen
 * to be unreachable by physical memory on any machine this kernel will ever
 * see, and syscall_mem_init() asserts that at boot rather than trusting it.
 *
 * END is the top of the lower canonical half, leaving a 64 TiB window.
 */
#define EXO_USER_VA_BASE  0x0000400000000000ULL
#define EXO_USER_VA_END   0x0000800000000000ULL

/* ---- Shared argument structs -------------------------------------------- */
/*
 * These cross the kernel/LibOS boundary, so their layout is ABI.  Explicit
 * reserved fields keep the size fixed and the members naturally aligned;
 * everything is little-endian x86_64 and the sizes are asserted below.
 */

#ifndef __ASSEMBLER__
/* exo_fb_acquire(info_out) — #4 */
typedef struct {
    uint64_t phys_addr;   /* framebuffer base, physical; LibOS maps it itself */
    uint32_t width;       /* pixels                                           */
    uint32_t height;      /* pixels                                           */
    uint32_t pitch;       /* bytes per scanline, may exceed width * bpp / 8   */
    uint8_t  bpp;         /* bits per pixel; 32 (BGRX8888) on QEMU today      */
    uint8_t  reserved[3]; /* zeroed by the kernel                             */
} exo_fb_info_t;
#endif /* __ASSEMBLER__ */

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
#ifndef __ASSEMBLER__
typedef struct {
    uint8_t pressed;      /* 1 = key down, 0 = key up            */
    uint8_t key;          /* decoded ps2_key_t index             */
    uint8_t modifiers;    /* EXO_MOD_* mask held when queued     */
    uint8_t reserved;     /* zeroed by the kernel                */
} exo_kbd_event_t;
#endif /* __ASSEMBLER__ */

#ifndef __ASSEMBLER__
/* exo_mouse_poll(state_out) — #7.  Deltas accumulate in the kernel between
 * calls and are reset to zero by each poll; buttons are a level, not a delta. */
typedef struct {
    int16_t dx;
    int16_t dy;
    uint8_t buttons;      /* bit 0 left, bit 1 right, bit 2 middle */
    uint8_t reserved;
} exo_mouse_state_t;

/* exo_memstat(stat_out) — #22.  Page-usage snapshot of the whole PMM, not
 * just the caller's own pages — introspection is deliberately unscoped, the
 * same way exo_pslist() below reports every live context rather than just
 * the caller's. */
typedef struct {
    uint32_t total_pages;   /* pages across every registered region         */
    uint32_t free_pages;    /* unallocated                                  */
    uint32_t kernel_pages;  /* allocated, PAGE_OWNER_KERNEL                 */
    uint32_t libos_pages;   /* allocated, owned by some LibOS context       */
    uint32_t region_count;  /* separate physical regions backing the pool   */
    uint32_t reserved;      /* zeroed by the kernel                         */
} exo_memstat_t;

/* exo_resstat(out) — #38 (SCRUM-232).  What is free right now, for a
 * launcher deciding whether a program's manifest (src/exo_pkg.h) fits. The
 * kernel only reports; grant / degrade / refuse is the launcher's policy.
 * Like exo_memstat, deliberately unscoped introspection. */
typedef struct {
    uint32_t free_pages;        /* unallocated pages (exo_memstat's free_pages) */
    uint32_t free_contexts;     /* CONTEXT_MAX minus live contexts              */
    uint32_t sound_voices_free; /* PCM mixer voices not currently sounding      */
    uint32_t fb_held;           /* 1 if some context holds the framebuffer      */
    uint32_t reserved[4];       /* zeroed by the kernel                         */
} exo_resstat_t;

/* exo_grantstat(ctx, out) — #39 (SCRUM-235).  What one context holds right
 * now, read from the tables that enforce it: page ownership, the framebuffer
 * and disk bindings, the sound rows.  A read-out, nothing is tracked for it.
 * Unscoped introspection like exo_resstat; a snapshot. */
typedef struct {
    uint32_t ctx_id;        /* the context asked about                       */
    uint32_t pages;         /* pages it owns (page_count_owned)              */
    uint32_t fb_owner;      /* context holding the framebuffer binding, 0 if
                             * none -- equal to ctx_id when it holds it      */
    uint32_t disk_base;     /* first sector of its assigned extent           */
    uint32_t disk_sectors;  /* 0 if it has no assigned extent                */
    uint32_t sound_voices;  /* mixer voices it started and that still hold
                             * a staging buffer                              */
    uint32_t sound_stream;  /* 1 if it holds the music stream                */
    uint32_t reserved;      /* zeroed by the kernel                          */
} exo_grant_t;

/* Flag for exo_launch_pkg's third argument: build the context and hand back
 * its id instead of switching to it.  The caller reads the grant with
 * exo_grantstat and then calls exo_launch_start(id). */
#define EXO_LAUNCH_HOLD   1u

/* exo_rtc_read(out) — #35.  Calendar date and time from the CMOS RTC, which
 * the kernel treats as UTC (no time zones).  unix_secs is seconds since
 * 1970-01-01 00:00:00 and is derived from the other fields. */
typedef struct {
    uint16_t year;          /* full year, e.g. 2026                         */
    uint8_t  month;         /* 1..12                                        */
    uint8_t  day;           /* 1..31                                        */
    uint8_t  hour;          /* 0..23                                        */
    uint8_t  minute;        /* 0..59                                        */
    uint8_t  second;        /* 0..59                                        */
    uint8_t  reserved;      /* zeroed by the kernel                         */
    uint64_t unix_secs;
} exo_rtc_time_t;

/* exo_pslist(out, max) — #23.  One entry per live LibOS context (the shell
 * included — it is context_create()'d like any other, SCRUM-178).
 * `state` mirrors context_state_t (src/context.h): 1=READY, 2=RUNNING,
 * 3=BLOCKED.  0 (CONTEXT_STATE_UNUSED) never appears — unused slots are
 * skipped, not reported. */
typedef struct {
    uint16_t id;
    uint8_t  state;
    uint8_t  reserved;      /* zeroed by the kernel                         */
    uint32_t page_count;    /* pages currently owned by this context        */
} exo_ps_info_t;

/* exo_diskstat(dev, out) — #31 (SCRUM-202, device SCRUM-244). Unscoped
 * introspection, same family as exo_memstat_t/exo_ps_info_t above: whether
 * block device `dev` exists, and its total 28-bit LBA sector count (0 if it
 * doesn't). No ownership check -- same reasoning as exo_memstat/exo_pslist,
 * and it touches no hardware: both fields are read from state the driver
 * latched at boot. */
typedef struct {
    uint8_t  present;       /* 1 if device `dev` exists, else 0             */
    uint8_t  reserved[3];   /* zeroed by the kernel                         */
    uint32_t total_sectors; /* 0 if !present                                */
} exo_diskstat_t;

/* #37 result (SCRUM-237): the sector extent assigned to the caller's volume. */
typedef struct {
    uint32_t base;    /* first LBA of the volume                        */
    uint32_t sectors; /* length in sectors, never 0                     */
} exo_diskextent_t;

/* EXO_PSLIST_MAX *is* CONTEXT_MAX (src/context.h) — included here rather than
 * restated as a separate literal, so the two can never drift the way they
 * did across SCRUM-193: that ticket raised VMM_MAX_ADDRESS_SPACES (and so
 * CONTEXT_MAX) from 4 to 16 without touching this file, and a hardcoded
 * EXO_PSLIST_MAX silently kept capping exo_pslist() at 3 live contexts
 * regardless of how many actually existed.
 *
 * context.h has no EXO_KERNEL gating of its own, nor do vmm.h/page_alloc.h
 * underneath it — all three are plain declarations, so this compiles
 * identically whether exo_syscall.h is being read as the kernel's own view
 * or a LibOS's. Kept inside this file's __ASSEMBLER__ guard regardless:
 * docker/scripts/build.sh's `-x assembler-with-cpp` pass over the .s probes
 * only ever wants #define numbers out of this header (see this file's own
 * top comment), and context.h's typedefs/prototypes are exactly the kind of
 * content that convention exists to keep out of assembly text. */
#include "context.h"
#define EXO_PSLIST_MAX CONTEXT_MAX
#endif /* __ASSEMBLER__ */

#ifndef __ASSEMBLER__
/* Layout is ABI — break it and the kernel and LibOS silently disagree. */
_Static_assert(sizeof(exo_fb_info_t)     == 24, "exo_fb_info_t layout is ABI");
_Static_assert(sizeof(exo_kbd_event_t)   ==  4, "exo_kbd_event_t layout is ABI");
_Static_assert(sizeof(exo_mouse_state_t) ==  6, "exo_mouse_state_t layout is ABI");
_Static_assert(sizeof(exo_memstat_t)     == 24, "exo_memstat_t layout is ABI");
_Static_assert(sizeof(exo_ps_info_t)     ==  8, "exo_ps_info_t layout is ABI");
_Static_assert(sizeof(exo_diskstat_t)    ==  8, "exo_diskstat_t layout is ABI");
_Static_assert(sizeof(exo_diskextent_t)  ==  8, "exo_diskextent_t layout is ABI");
_Static_assert(sizeof(exo_rtc_time_t)    == 16, "exo_rtc_time_t layout is ABI");
_Static_assert(sizeof(exo_resstat_t)     == 32, "exo_resstat_t layout is ABI");
_Static_assert(sizeof(exo_grant_t)       == 32, "exo_grant_t layout is ABI");
#endif /* __ASSEMBLER__ */

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

/* #1 — release a page from exo_page_alloc.  0 on success, -EXO_EPERM if paddr
 * is allocated but owned by the kernel or another LibOS, -EXO_EINVAL if it is
 * unaligned, out of range, or already free. */
static inline int64_t exo_page_free(uint64_t paddr)
{
    return exo_syscall1(EXO_SYS_PAGE_FREE, paddr);
}

/* #2 — map paddr at vaddr in the caller's address space.  flags is a mask of
 * EXO_PAGE_*.  Ownership-checked (docs/syscall_spec.md §3.3): paddr must be a
 * page the caller owns, or framebuffer memory it has acquired.  Returns 0,
 * -EXO_EINVAL (misaligned address or unknown flag bit), -EXO_EPERM (vaddr
 * outside [EXO_USER_VA_BASE, EXO_USER_VA_END), or a paddr the caller does not
 * own) or -EXO_ENOMEM (no page left for an intermediate page table). */
static inline int64_t exo_page_map(uint64_t vaddr, uint64_t paddr,
                                   uint32_t flags)
{
    return exo_syscall3(EXO_SYS_PAGE_MAP, vaddr, paddr, (uint64_t)flags);
}

/* #3 — remove the mapping at vaddr.  Does not free the underlying page; call
 * exo_page_free for that.  Returns 0, -EXO_EINVAL (misaligned vaddr, or
 * nothing mapped there), -EXO_EPERM (vaddr outside the window, or the mapping
 * is of a page belonging to the kernel or another LibOS) or -EXO_ENOMEM. */
static inline int64_t exo_page_unmap(uint64_t vaddr)
{
    return exo_syscall1(EXO_SYS_PAGE_UNMAP, vaddr);
}

/* #4 — claim the framebuffer and describe it, binding it to the caller
 * (secure binding, docs/syscall_spec.md §3.3).  The LibOS maps the reported
 * physical range itself with exo_page_map, which from SCRUM-154 on requires
 * holding this binding.  Returns 0 (also for a re-acquire by the current
 * owner), -EXO_EBUSY if another LibOS holds it, -EXO_EFAULT for a bad
 * info_out, or -EXO_ENODEV on a machine with no framebuffer.  The binding is
 * released by exo_exit.  Used by DG_Init. */
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

/* #9-#16 have no stubs: they were reserved for kernel file syscalls that
 * were never bound. File I/O is LibOS code -- exo_file_* lives in
 * src/libos_fs/exo_file.h, over ExoFS (SCRUM-44). */

/* #17 — start a PC speaker tone.  Returns immediately; the kernel stops the
 * tone after dur_ms.  0, or -EXO_EINVAL for freq outside 19..20000 Hz or
 * dur_ms outside 1..10000 (SCRUM-100, src/syscall_sound.c).  Last tone wins. */
static inline int64_t exo_sound_tone(uint32_t freq, uint32_t dur_ms)
{
    return exo_syscall2(EXO_SYS_SOUND_TONE, (uint64_t)freq, (uint64_t)dur_ms);
}

/* #18 — silence the speaker now.  0 (also if nothing is sounding), or
 * -EXO_EPERM if another context started the tone that is sounding. */
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

/*
 * #21 — launch the ring-3 LibOS app named by `app_id` (an EXO_LAUNCH_APP_*
 * value) as its own context and switch to it immediately
 * (src/syscall_launch.c, SCRUM-184).
 *
 * Replaces exo_launch_wad_viewer()/_clock()/_snake()/_doom(), which were four
 * no-argument wrappers over four syscall numbers. One wrapper over one
 * number, because the thing that varied was never the call -- it was which
 * blob the kernel picked.
 *
 * Calling convention, unchanged from those four: this does not return control
 * to the caller when it succeeds. The launch arms a context switch and the
 * syscall epilogue takes it, so the next thing that runs is the new app. The
 * call appears to return 0 only later, once something switches back to the
 * caller -- the app's own exo_yield() after exiting via EXO_SYS_EXIT, or
 * Ctrl+Tab (SCRUM-111) for an app that never yields, such as the clock.
 *
 * A negative EXO_E* comes back immediately, with the caller still running
 * uninterrupted, if the launch failed before the switch was armed:
 *
 *   -EXO_EINVAL  app_id is not a valid EXO_LAUNCH_APP_* (new to SCRUM-184 --
 *                with a number per app, an unknown app was an unknown
 *                syscall and the dispatcher answered -EXO_ENOSYS)
 *   -EXO_ENODEV  the app needs the WAD module and there is none
 *   -EXO_ENOMEM  no room for the image or no free context row
 */
static inline int64_t exo_launch(uint64_t app_id)
{
    return exo_syscall1(EXO_SYS_LAUNCH, app_id);
}

/* #22 — page-usage snapshot of the whole PMM (docs/syscall_spec.md §3.2).
 * Returns 0 with *stat_out filled, or -EXO_EFAULT if
 * [stat_out, stat_out + sizeof(exo_memstat_t)) is not entirely inside the
 * LibOS window and mapped writable. */
static inline int64_t exo_memstat(exo_memstat_t *stat_out)
{
    return exo_syscall1(EXO_SYS_MEMSTAT, (uint64_t)(uintptr_t)stat_out);
}

/* #23 — list every live LibOS context (the caller included) into `out`,
 * an array of at least `max` exo_ps_info_t entries; `max` must not exceed
 * EXO_PSLIST_MAX. Returns the number of entries written (0..max), or
 * -EXO_EFAULT for a bad `out` range, -EXO_EINVAL if max > EXO_PSLIST_MAX. */
static inline int64_t exo_pslist(exo_ps_info_t *out, uint32_t max)
{
    return exo_syscall2(EXO_SYS_PSLIST, (uint64_t)(uintptr_t)out,
                        (uint64_t)max);
}

/* #24 — read `count` consecutive 512-byte sectors of block device `dev`
 * (0 = the system disk, SCRUM-244) starting at 28-bit LBA `lba` into `buf`
 * (at least `count * 512` bytes). Returns `count` on success, or
 * -EXO_ENODEV (no such device), -EXO_EINVAL (count == 0 is NOT an
 * error and returns 0; count over EXO_DISK_MAX_SECTORS or lba+count
 * overflowing 28-bit LBA space are), -EXO_EFAULT (buf outside the LibOS
 * window or not mapped writable), -EXO_EBUSY (caller does not hold a
 * binding on this device -- see exo_disk_acquire, SCRUM-188) or -EXO_EIO (a
 * sector faulted partway through). */
static inline int64_t exo_disk_read(uint32_t dev, uint32_t lba, void *buf,
                                    uint32_t count)
{
    return exo_syscall4(EXO_SYS_DISK_READ, (uint64_t)dev, (uint64_t)lba,
                        (uint64_t)(uintptr_t)buf, (uint64_t)count);
}

/* #25 — write `count` consecutive 512-byte sectors of device `dev` starting
 * at 28-bit LBA `lba` from `buf`. Same return/error contract as
 * exo_disk_read, with `buf` checked readable rather than writable. */
static inline int64_t exo_disk_write(uint32_t dev, uint32_t lba,
                                     const void *buf, uint32_t count)
{
    return exo_syscall4(EXO_SYS_DISK_WRITE, (uint64_t)dev, (uint64_t)lba,
                        (uint64_t)(uintptr_t)buf, (uint64_t)count);
}

/* #26 — bind sectors [base, base + count) of device `dev` to the caller
 * (SCRUM-188, ranges SCRUM-237, device SCRUM-244). A caller may hold one
 * extent on each device at once; extents of different devices never
 * conflict. count == 0 (and base == 0) claims the whole device, which is
 * what the argument-less call did before ranges existed. Must succeed before
 * exo_disk_read/exo_disk_write will do anything for this caller, and those
 * then refuse any sector outside [base, base + count). Returns 0 (including a
 * re-acquire of the same extent), -EXO_EBUSY if the extent overlaps another
 * context's on this device or the caller already holds a different one of
 * it, -EXO_EINVAL for a malformed extent, -EXO_EPERM if the caller has an
 * assigned volume and this is not it, -EXO_ENOMEM if the extent table is
 * full, or -EXO_ENODEV if there is no such device. */
static inline int64_t exo_disk_acquire(uint32_t dev, uint32_t base,
                                       uint32_t count)
{
    return exo_syscall3(EXO_SYS_DISK_ACQUIRE, (uint64_t)dev, (uint64_t)base,
                        (uint64_t)count);
}

/* #27 — give the binding of device `dev` back (SCRUM-189, device SCRUM-244);
 * any other device the caller holds stays held. The voluntary counterpart to
 * exo_disk_acquire, and the only way a LibOS can perform phase 2 of the
 * revocation protocol for this resource (docs/syscall_spec.md §3.6): without
 * it, `disk_binding_release()` is reachable from ring 0 only, so the disk
 * could change hands solely by forced reclaim.
 *
 * Always returns 0 — releasing a binding this caller does not hold is a
 * no-op, not an error, for the same reason a re-acquire by the current owner
 * is success: a LibOS unwinding after a failed acquire should not have to
 * remember which one it was. Flush anything cached above this first; the
 * kernel does not know the binding meant anything (ExoFS calls
 * exofs_unmount() before exofs_bdev_release() for exactly this reason). */
static inline int64_t exo_disk_release(uint32_t dev)
{
    return exo_syscall1(EXO_SYS_DISK_RELEASE, (uint64_t)dev);
}

/* #31 — presence and total 28-bit LBA sector count of block device `dev`
 * (SCRUM-202, device SCRUM-244). No acquire needed first: unscoped
 * introspection like exo_memstat/exo_pslist, not a resource grant. An absent
 * device is not an error -- it reports present == 0 -- so the disks are
 * enumerated by asking every dev below EXO_DISK_MAX_DEVICES (device 0 can be
 * absent while others are present). Returns 0
 * with *out filled, -EXO_EINVAL if dev >= EXO_DISK_MAX_DEVICES, or
 * -EXO_EFAULT if [out, out + sizeof(exo_diskstat_t)) is not entirely inside
 * the LibOS window and mapped writable. */
static inline int64_t exo_diskstat(uint32_t dev, exo_diskstat_t *out)
{
    return exo_syscall2(EXO_SYS_DISK_STAT, (uint64_t)dev,
                        (uint64_t)(uintptr_t)out);
}

/* #37 — the sector extent this program's package asked for and the kernel
 * assigned at launch (SCRUM-237). Pass exactly this to exo_disk_acquire and
 * format/mount ExoFS at .base. Returns 0 with *out filled, -EXO_ENOENT if the
 * caller has no volume (its package did not request one, or it was not
 * launched from a package), or -EXO_EFAULT if [out, out + 8) is not mapped
 * writable in the LibOS window. */
static inline int64_t exo_disk_extent(exo_diskextent_t *out)
{
    return exo_syscall1(EXO_SYS_DISK_EXTENT, (uint64_t)(uintptr_t)out);
}

/* #28 — queue `num_samples` bytes of 8-bit *unsigned* mono PCM at `buf`,
 * recorded at `rate_hz`, on one of the mixer's voices (SCRUM-213). Returns at
 * once: the samples are copied into kernel memory and played by DMA, so `buf`
 * is the caller's again the moment this returns.
 *
 * `rate_hz` is the rate the samples were recorded at, NOT the hardware's --
 * the mixer resamples each voice to the stream's own rate, which is why a DMX
 * lump can be handed over exactly as doom_dmx_parse() reports it. `vol` is
 * 0..127, `sep` 0..255 with 128 centred, and `priority` follows Doom's
 * sfxinfo_t convention where a LOWER number is MORE important.
 *
 * Returns a non-negative voice handle for exo_sound_pcm_stop, or:
 *   -EXO_EINVAL   num_samples 0 or over SOUND_PCM_MAX_SAMPLES, or an
 *                 unplayable rate_hz
 *   -EXO_ENODEV   this machine has no audio controller, or the output stream
 *                 refused to start
 *   -EXO_EFAULT   [buf, buf+num_samples) is not entirely inside the LibOS
 *                 window and mapped
 *   -EXO_EBUSY    every voice is busy with sound at least as important as
 *                 this one; nothing already playing was disturbed
 *   -EXO_ENOMEM   no contiguous kernel pages to copy the samples into
 * docs/syscall_spec.md §3.2 #28 and §3.8. */
static inline int64_t exo_sound_pcm(const void *buf, uint32_t num_samples,
                                    uint32_t rate_hz, int vol, int sep,
                                    int priority)
{
    return exo_syscall6(EXO_SYS_SOUND_PCM, (uint64_t)(uintptr_t)buf,
                        (uint64_t)num_samples, (uint64_t)rate_hz,
                        (uint64_t)(int64_t)vol, (uint64_t)(int64_t)sep,
                        (uint64_t)(int64_t)priority);
}

/* #29 — stop the voice `handle` names. 0 on success, and also for a handle
 * whose sound has already finished or been taken over by a more important one
 * (there is nothing left to stop, which is not an error). -EXO_EPERM if the
 * voice is playing another context's sound, -EXO_EINVAL for a negative
 * handle, which was never issued by anyone. */
static inline int64_t exo_sound_pcm_stop(int handle)
{
    return exo_syscall1(EXO_SYS_SOUND_PCM_STOP, (uint64_t)(int64_t)handle);
}

/* #30 — re-place the voice `handle` names: `vol` 0..127 and `sep` 0..255 with
 * 128 centred, the same ranges #28 takes, applied through the same panning law.
 * Changes how the voice is heard and nothing else -- not its priority, not how
 * far through its sample it is.
 *
 * 0 on success, and also for a handle whose sound has already finished or been
 * taken over: Doom retunes every channel it believes is active, a tic or so
 * before it notices one has ended, so that is the ordinary case rather than an
 * error. -EXO_EPERM if the voice is playing another context's sound (left
 * untouched), -EXO_EINVAL for a negative handle. docs/syscall_spec.md §3.2 #30
 * and §3.5b. */
static inline int64_t exo_sound_pcm_params(int handle, int vol, int sep)
{
    return exo_syscall3(EXO_SYS_SOUND_PCM_PARAMS, (uint64_t)(int64_t)handle,
                        (uint64_t)(int64_t)vol, (uint64_t)(int64_t)sep);
}

/* #32 -- queue up to `frames` interleaved 16-bit stereo frames (4 bytes each,
 * already at the mixer's rate) onto the music stream, binding it to the caller
 * on the first call.  Never blocks: a ring short of room takes fewer frames
 * than offered, and a full one takes none.
 *
 * On success the result packs two counts: the low 32 bits are the frames
 * accepted (0..frames), the high 32 the frames of room left AFTER this write,
 * so a refill costs one ring transition.  Use the exo_sound_stream_accepted()
 * and exo_sound_stream_space() helpers below.  Errors are negative:
 *   -EXO_EINVAL   frames 0 or over SOUND_STREAM_MAX_FRAMES
 *   -EXO_ENODEV   no audio controller, or the output stream refused to start
 *   -EXO_EFAULT   [buf, buf + frames * 4) not entirely inside the LibOS window
 *                 and mapped
 *   -EXO_EBUSY    another context holds the stream
 *   -EXO_ENOMEM   no contiguous kernel pages for the ring (first call only)
 * docs/syscall_spec.md §3.2 #32 and §3.5b. */
static inline int64_t exo_sound_stream_write(const void *buf, uint32_t frames)
{
    return exo_syscall2(EXO_SYS_SOUND_STREAM_WRITE, (uint64_t)(uintptr_t)buf,
                        (uint64_t)frames);
}

/* Decode a non-negative exo_sound_stream_write result. */
static inline uint32_t exo_sound_stream_accepted(int64_t r)
{
    return (uint32_t)((uint64_t)r & 0xFFFFFFFFu);
}
static inline uint32_t exo_sound_stream_space(int64_t r)
{
    return (uint32_t)((uint64_t)r >> 32);
}

/* #33 -- drop whatever is buffered, stop the stream and release it (and its
 * kernel pages) so another context may bind.  0, also when nothing is held;
 * -EXO_EPERM if another context holds it.  exo_exit does this implicitly. */
static inline int64_t exo_sound_stream_stop(void)
{
    return exo_syscall0(EXO_SYS_SOUND_STREAM_STOP);
}

/* #34 -- set the music level, 0..127 (clamped).  Unowned: any context may
 * call it, with or without the stream bound, and the level persists across
 * bind/release -- Doom sets it before the first song exists.  Always 0. */
static inline int64_t exo_sound_stream_volume(int vol)
{
    return exo_syscall1(EXO_SYS_SOUND_STREAM_VOLUME, (uint64_t)(int64_t)vol);
}

/* #35 -- read the wall clock into *out.  0 on success; -EXO_EFAULT if
 * [out, out + sizeof(exo_rtc_time_t)) is not entirely inside the LibOS window
 * and mapped writable; -EXO_ENODEV if the machine has no CMOS RTC; -EXO_EIO
 * if the RTC returned values that are not a valid date. */
static inline int64_t exo_rtc_read(exo_rtc_time_t *out)
{
    return exo_syscall1(EXO_SYS_RTC_READ, (uint64_t)(uintptr_t)out);
}

/* #36 -- build and switch to the LibOS packaged at [pkg, pkg + len)
 * (src/exo_pkg.h, SCRUM-229).  The buffer is the whole package -- header, code
 * bytes, data bytes -- exactly as it sits in a file; the kernel validates it,
 * copies the sections into pages the new context owns, and the buffer is the
 * caller's again as soon as this returns.  Same calling convention as
 * exo_launch(): on success control does not come back until something
 * switches back to the caller.
 *
 *   -EXO_EINVAL  len is 0 or over EXO_LAUNCH_PKG_MAX_LEN, or the package is
 *                malformed (bad magic/version, a length over the loader's
 *                caps, a bad manifest field, or shorter than its header
 *                claims)
 *   -EXO_EFAULT  [pkg, pkg + len) is not entirely inside the LibOS window and
 *                mapped
 *   -EXO_ENOMEM  no room for the image or no free context row
 *
 * With EXO_LAUNCH_HOLD in `flags` the context is built but not entered: the
 * call returns its id (> 0) at once, and the program does not run until
 * exo_launch_start(id).  Any other flag bit is -EXO_EINVAL.
 */
static inline int64_t exo_launch_pkg(const void *pkg, uint64_t len,
                                     uint64_t flags)
{
    return exo_syscall3(EXO_SYS_LAUNCH_PKG, (uint64_t)(uintptr_t)pkg, len, flags);
}

/* #40 — enter a context #36 built with EXO_LAUNCH_HOLD.  Like exo_launch, on
 * success control does not come back until something switches back.
 *
 *   -EXO_ENOENT  no held context with that id (never held, already started,
 *                or since destroyed)
 *   -EXO_EINVAL  the switch could not be armed; the held context is destroyed
 */
static inline int64_t exo_launch_start(uint64_t ctx)
{
    return exo_syscall1(EXO_SYS_LAUNCH_START, ctx);
}

/* #39 — fill *out with what `ctx` was granted (SCRUM-235).
 *
 *   0            *out filled
 *   -EXO_ENOENT  no live context with that id
 *   -EXO_EFAULT  [out, out + sizeof(exo_grant_t)) is not entirely inside the
 *                LibOS window and mapped writable
 */
static inline int64_t exo_grantstat(uint64_t ctx, exo_grant_t *out)
{
    return exo_syscall2(EXO_SYS_GRANTSTAT, ctx, (uint64_t)(uintptr_t)out);
}

/* #38 — fill *out with what is free right now (SCRUM-232).  A snapshot: it
 * can be stale by the time the launch happens, which is why #36 keeps its own
 * cheap page check as a backstop.
 *
 *   0            *out filled
 *   -EXO_EFAULT  [out, out + sizeof(exo_resstat_t)) is not entirely inside the
 *                LibOS window and mapped writable
 */
static inline int64_t exo_resstat(exo_resstat_t *out)
{
    return exo_syscall1(EXO_SYS_RESSTAT, (uint64_t)(uintptr_t)out);
}

#endif /* !EXO_KERNEL */
