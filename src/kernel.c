#include <stdint.h>
#include "multiboot2.h"
#include "serial.h"
#include "memory.h"
#include "mmap.h"
#include "page_alloc.h"
#include "vmm.h"

#include "idt.h"
#include "pic.h"
#include "pit.h"
#include "ps2.h"
#include "sleep.h"
#include "fb.h"
#include "fb_console.h"
#include "syscall.h"
#include "syscall_mem.h"
#include "syscall_fb.h"
#include "fb_binding.h"
#include "revoke.h"
#include "vmm.h"
#include "exo_syscall.h"

extern void irq0_stub();
extern void irq1_stub();
extern void kbd_init();

#ifdef TESTING
extern int run_tests(void);
#endif

static inline void qemu_exit(uint32_t code) {
    __asm__ volatile ("outl %0, %1" : : "a"(code), "Nd"(0xF4));
}

// ── Console number formatters ─────────────────────────────────────────────

static void fbcon_write_hex64(fb_console_t *con, uint64_t val) {
    const char hex[] = "0123456789abcdef";
    char buf[17];
    buf[16] = '\0';
    for (int i = 15; i >= 0; i--) {
        buf[i] = hex[val & 0xF];
        val >>= 4;
    }
    fbcon_write(con, buf);
}

static void fbcon_write_hex32(fb_console_t *con, uint32_t val) {
    const char hex[] = "0123456789abcdef";
    char buf[9];
    buf[8] = '\0';
    for (int i = 7; i >= 0; i--) {
        buf[i] = hex[val & 0xF];
        val >>= 4;
    }
    fbcon_write(con, buf);
}

static void fbcon_write_u32(fb_console_t *con, uint32_t val) {
    if (val == 0) { fbcon_write(con, "0"); return; }
    char buf[11];
    buf[10] = '\0';
    int i = 10;
    while (val > 0) { buf[--i] = '0' + (val % 10); val /= 10; }
    fbcon_write(con, &buf[i]);
}

static void fbcon_write_memsize(fb_console_t *con, uint64_t bytes) {
    if (bytes >= 1024ULL * 1024ULL) {
        fbcon_write_u32(con, (uint32_t)(bytes / (1024ULL * 1024ULL)));
        fbcon_write(con, " MB");
    } else if (bytes >= 1024ULL) {
        fbcon_write_u32(con, (uint32_t)(bytes / 1024ULL));
        fbcon_write(con, " KB");
    } else {
        fbcon_write_u32(con, (uint32_t)bytes);
        fbcon_write(con, " B");
    }
}

static void write_ts(fb_console_t *con, uint32_t ms) {
    uint32_t s = ms / 1000;
    uint32_t f = ms % 1000;
    char buf[9];
    buf[8] = '\0';
    buf[7] = '0' + f % 10; f /= 10;
    buf[6] = '0' + f % 10; f /= 10;
    buf[5] = '0' + f % 10;
    buf[4] = '.';
    buf[3] = '0' + s % 10; s /= 10;
    buf[2] = '0' + s % 10; s /= 10;
    buf[1] = '0' + s % 10; s /= 10;
    buf[0] = '0' + s % 10;
    fbcon_write(con, buf);
}

static void log_prefix(fb_console_t *con, uint32_t ms) {
    fbcon_set_color(con, 80, 200, 80, 0, 0, 0);
    fbcon_write(con, "[");
    write_ts(con, ms);
    fbcon_write(con, "] ");
    fbcon_set_color(con, 220, 220, 220, 0, 0, 0);
}

static void klog(fb_console_t *con, uint32_t ms, const char *msg) {
    log_prefix(con, ms);
    fbcon_write(con, msg);
    fbcon_write(con, "\n");
}

// ── Memory map helpers ────────────────────────────────────────────────────

static const char *mmap_type_name(uint32_t type) {
    switch (type) {
    case MB2_MMAP_AVAILABLE:    return "usable";
    case MB2_MMAP_RESERVED:     return "reserved";
    case MB2_MMAP_ACPI_RECLAIM: return "acpi reclaimable";
    case MB2_MMAP_ACPI_NVS:     return "acpi nvs";
    case MB2_MMAP_BADRAM:       return "bad ram";
    default:                    return "unknown";
    }
}

static void mmap_type_color(fb_console_t *con, uint32_t type) {
    switch (type) {
    case MB2_MMAP_AVAILABLE:
        fbcon_set_color(con, 80, 210, 80, 0, 0, 0); break;
    case MB2_MMAP_ACPI_RECLAIM:
    case MB2_MMAP_ACPI_NVS:
        fbcon_set_color(con, 220, 190, 60, 0, 0, 0); break;
    case MB2_MMAP_BADRAM:
        fbcon_set_color(con, 230, 50, 50, 0, 0, 0); break;
    default:
        fbcon_set_color(con, 140, 140, 140, 0, 0, 0); break;
    }
}

static void print_mmap(fb_console_t *con) {
    uint32_t count;
    const mmap_region_t *regions = mmap_get_regions(&count);

    klog(con, 0, "BIOS-provided physical RAM map:");

    uint64_t total_usable = 0;
    for (uint32_t i = 0; i < count; i++) {
        log_prefix(con, 0);
        mmap_type_color(con, regions[i].type);
        fbcon_write(con, "  [mem 0x");
        fbcon_write_hex64(con, regions[i].base);
        fbcon_write(con, "-0x");
        fbcon_write_hex64(con, regions[i].base + regions[i].length - 1);
        fbcon_write(con, "]  ");
        fbcon_write_memsize(con, regions[i].length);
        fbcon_write(con, "  ");
        fbcon_write(con, mmap_type_name(regions[i].type));
        fbcon_write(con, "\n");

        if (regions[i].type == MB2_MMAP_AVAILABLE)
            total_usable += regions[i].length;
    }

    fbcon_set_color(con, 220, 220, 220, 0, 0, 0);
    log_prefix(con, 0);
    fbcon_write(con, "Total usable RAM: ");
    fbcon_set_color(con, 80, 210, 80, 0, 0, 0);
    fbcon_write_memsize(con, total_usable);
    fbcon_set_color(con, 220, 220, 220, 0, 0, 0);
    fbcon_write(con, "\n");
}

// ── Page ownership self-check (SCRUM-152) ──────────────────────────────────
//
// A *visible* companion to the serial-only KUnit suite: it drives the real
// exo_page_alloc / exo_page_free dispatch path at boot and renders the result
// to the framebuffer console, with a green/red status swatch in the top-right
// corner.  It demonstrates the secure-binding guarantee — a LibOS cannot free a
// page it does not own — on screen rather than only in CI serial output.

static int ownership_check(fb_console_t *con, const char *label, int ok) {
    log_prefix(con, 0);
    fbcon_write(con, label);
    if (ok) {
        fbcon_set_color(con, 80, 210, 80, 0, 0, 0);
        fbcon_write(con, "PASS");
    } else {
        fbcon_set_color(con, 230, 50, 50, 0, 0, 0);
        fbcon_write(con, "FAIL");
    }
    fbcon_set_color(con, 220, 220, 220, 0, 0, 0);
    fbcon_write(con, "\n");
    return ok;
}

// ── Framebuffer secure binding self-check (SCRUM-154) ─────────────────────
//
// The same idea one resource up: the framebuffer is bound to one LibOS at a
// time, and mapping its physical pages requires holding that binding.  Runs on
// the real exo_fb_acquire dispatch path and leaves the framebuffer unbound, so
// the kernel console below keeps the screen for the rest of the boot.

// A second, distinct context id — the "another LibOS" the binding must refuse.
#define DEMO_OTHER_LIBOS ((page_owner_t)(PAGE_OWNER_LIBOS + 1))

static int run_fb_binding_demo(fb_console_t *con) {
    exo_fb_info_t info = { 0, 0, 0, 0, 0, { 0, 0, 0 } };
    int all = 1;

    klog(con, 0, "Framebuffer binding self-check (SCRUM-154):");

    // 1. Acquire binds the framebuffer to the calling context.
    int64_t r_acq = exo_syscall_dispatch(EXO_SYS_FB_ACQUIRE,
                                         (uint64_t)(uintptr_t)&info,
                                         0, 0, 0, 0, 0);
    log_prefix(con, 0);
    fbcon_write(con, "  exo_fb_acquire -> phys 0x");
    fbcon_set_color(con, 100, 180, 255, 0, 0, 0);
    fbcon_write_hex64(con, info.phys_addr);
    fbcon_set_color(con, 220, 220, 220, 0, 0, 0);
    fbcon_write(con, "\n");
    all &= ownership_check(con, "  framebuffer bound to LibOS              ",
                           r_acq == 0 &&
                           fb_binding_owner() == syscall_current_context());

    // 2. The owner may map framebuffer pages; nobody else may (SCRUM-153 asks
    //    fb_binding_check_map before consulting per-page ownership).
    all &= ownership_check(con, "  owner may map FB pages                  ",
                           fb_binding_check_map(info.phys_addr,
                                                syscall_current_context())
                           == FB_MAP_ALLOW);
    all &= ownership_check(con, "  foreign FB map rejected (EPERM)         ",
                           fb_binding_check_map(info.phys_addr,
                                                DEMO_OTHER_LIBOS)
                           == FB_MAP_DENY);

    // 3. With another context holding it, acquire answers -EXO_EBUSY.
    fb_binding_release(syscall_current_context());
    fb_binding_acquire(DEMO_OTHER_LIBOS);
    int64_t r_busy = exo_syscall_dispatch(EXO_SYS_FB_ACQUIRE,
                                          (uint64_t)(uintptr_t)&info,
                                          0, 0, 0, 0, 0);
    all &= ownership_check(con, "  second acquirer rejected (EBUSY)        ",
                           r_busy == -EXO_EBUSY);

    // 4. Reclamation (what SCRUM-155's exo_exit will do) frees it again.
    fb_binding_release(DEMO_OTHER_LIBOS);
    all &= ownership_check(con, "  release frees the binding               ",
                           fb_binding_owner() == PAGE_OWNER_FREE);

    return all;
}

// ── Resource revocation self-check (SCRUM-156) ────────────────────────────
//
// The third leg of the model: what the kernel granted, the kernel can take
// back.  Walks the protocol in docs/syscall_spec.md §3.6 — request, comply,
// force, sweep — on a real page and on the framebuffer, and leaves nothing
// bound and nothing allocated, so the console below keeps the screen.

static int run_revocation_demo(fb_console_t *con) {
    int all = 1;

    klog(con, 0, "Resource revocation self-check (SCRUM-156):");

    // 1. The request is an ask, not a seizure: the page stays the owner's and
    //    stays usable.  Side effects are sequenced before the assertion so a
    //    failing step cannot short-circuit the ones that clean up after it.
    void *p = alloc_page_owned(DEMO_OTHER_LIBOS);
    revoke_res_t res = revoke_res_page((uint64_t)(uintptr_t)p);
    int marked = (p != NULL) && revoke_request(DEMO_OTHER_LIBOS, res) == REVOKE_OK;
    all &= ownership_check(con, "  request marks, owner keeps the page     ",
                           marked && revoke_pending(res) &&
                           page_owner(p) == DEMO_OTHER_LIBOS);

    // 2. Compliance: the owner returns a marked page through the ordinary free
    //    path, and the mark goes with it.
    int complied = free_page_owned(p, DEMO_OTHER_LIBOS) == PAGE_FREE_OK;
    all &= ownership_check(con, "  owner returns it, mark clears           ",
                           complied && !revoke_pending(res));
    all &= ownership_check(con, "  force then finds nothing to take        ",
                           revoke_force(DEMO_OTHER_LIBOS, res) == REVOKE_RETURNED);

    // 3. A LibOS that ignores the ask loses the page anyway.
    void *kept = alloc_page_owned(DEMO_OTHER_LIBOS);
    revoke_res_t kres = revoke_res_page((uint64_t)(uintptr_t)kept);
    int forced = (kept != NULL) &&
                 revoke_force(DEMO_OTHER_LIBOS, kres) == REVOKE_OK;
    all &= ownership_check(con, "  ignored request is forced              ",
                           forced && page_owner(kept) == PAGE_OWNER_FREE);

    // 4. Revocation is scoped to the context it names: reclaiming for one
    //    LibOS must never free a page another one holds.
    void *peer = alloc_page_owned(syscall_current_context());
    revoke_res_t pres = revoke_res_page((uint64_t)(uintptr_t)peer);
    int spared = (peer != NULL) &&
                 revoke_force(DEMO_OTHER_LIBOS, pres) == REVOKE_RETURNED &&
                 page_owner(peer) == syscall_current_context();
    all &= ownership_check(con, "  peer's page left alone                  ", spared);
    (void)free_page_owned(peer, syscall_current_context());

    // 5. The v1 policy: exo_exit's sweep (SCRUM-155) takes every page the
    //    context holds plus the framebuffer, in one call.
    (void)alloc_page_owned(DEMO_OTHER_LIBOS);
    (void)alloc_page_owned(DEMO_OTHER_LIBOS);
    fb_binding_acquire(DEMO_OTHER_LIBOS);
    uint32_t swept = revoke_all(DEMO_OTHER_LIBOS);
    all &= ownership_check(con, "  exit sweep reclaims pages + screen      ",
                           swept == 3 &&
                           page_count_owned(DEMO_OTHER_LIBOS) == 0 &&
                           fb_binding_owner() == PAGE_OWNER_FREE);

    // 6. And the kernel's own pages are not sweepable by anyone.
    uint32_t kernel_pages = page_count_owned(PAGE_OWNER_KERNEL);
    all &= ownership_check(con, "  kernel pages are not revocable          ",
                           revoke_all(PAGE_OWNER_KERNEL) == 0 &&
                           page_count_owned(PAGE_OWNER_KERNEL) == kernel_pages);

    const revoke_record_t *rec = revoke_record();
    log_prefix(con, 0);
    fbcon_write(con, "  repossession record: ");
    fbcon_set_color(con, 100, 180, 255, 0, 0, 0);
    fbcon_write_u32(con, rec->requested);
    fbcon_set_color(con, 220, 220, 220, 0, 0, 0);
    fbcon_write(con, " asked, ");
    fbcon_set_color(con, 100, 180, 255, 0, 0, 0);
    fbcon_write_u32(con, rec->returned);
    fbcon_set_color(con, 220, 220, 220, 0, 0, 0);
    fbcon_write(con, " returned, ");
    fbcon_set_color(con, 100, 180, 255, 0, 0, 0);
    fbcon_write_u32(con, rec->forced);
    fbcon_set_color(con, 220, 220, 220, 0, 0, 0);
    fbcon_write(con, " taken\n");

    return all;
}

// ── Address-space mapping self-check (SCRUM-35 / -153) ────────────────────
//
// The fourth resource operation: a LibOS builds its own address space, one
// page at a time, out of pages it owns — and cannot build it out of anything
// else.  Runs on the real exo_page_map / exo_page_unmap dispatch path and
// leaves the address space exactly as it found it.

static int run_page_map_demo(fb_console_t *con) {
    int all = 1;

    klog(con, 0, "Address-space mapping self-check (SCRUM-35):");

    // A scratch virtual address in the LibOS window, well clear of the
    // kernel's identity map.
    const uint64_t scratch = EXO_USER_VA_BASE + 0x30000000ULL;

    // 1. A page the caller owns can be mapped where the caller asks, and the
    //    mapping is real: a write through the virtual address lands in the
    //    physical page, seen here through the identity map.
    int64_t p = exo_syscall_dispatch(EXO_SYS_PAGE_ALLOC, 0, 0, 0, 0, 0, 0);
    int64_t r_map = exo_syscall_dispatch(EXO_SYS_PAGE_MAP, scratch, (uint64_t)p,
                                         EXO_PAGE_READ | EXO_PAGE_WRITE, 0, 0, 0);
    log_prefix(con, 0);
    fbcon_write(con, "  exo_page_map 0x");
    fbcon_set_color(con, 100, 180, 255, 0, 0, 0);
    fbcon_write_hex64(con, (uint64_t)p);
    fbcon_set_color(con, 220, 220, 220, 0, 0, 0);
    fbcon_write(con, " -> 0x");
    fbcon_set_color(con, 100, 180, 255, 0, 0, 0);
    fbcon_write_hex64(con, scratch);
    fbcon_set_color(con, 220, 220, 220, 0, 0, 0);
    fbcon_write(con, "\n");

    if (p > 0 && r_map == 0)
        *(volatile uint64_t *)(uintptr_t)scratch = 0x5CA1AB1E5CA1AB1EULL;

    all &= ownership_check(con, "  owned page mapped and writable         ",
                           p > 0 && r_map == 0 &&
                           *(volatile uint64_t *)(uintptr_t)p
                               == 0x5CA1AB1E5CA1AB1EULL);

    // 2. Another context's page is not mappable — the hole SCRUM-153 closes.
    void *theirs = alloc_page_owned(DEMO_OTHER_LIBOS);
    int64_t r_foreign = exo_syscall_dispatch(EXO_SYS_PAGE_MAP, scratch,
                                             (uint64_t)(uintptr_t)theirs,
                                             EXO_PAGE_WRITE, 0, 0, 0);
    // The NULL check is what makes this a test of foreign-page rejection: on
    // an exhausted pool `theirs` is 0, page_owner(0) answers PAGE_OWNER_FREE,
    // and the -EPERM below would be earned by an unowned address instead.
    all &= ownership_check(con, "  foreign page not mappable (EPERM)      ",
                           theirs != NULL && r_foreign == -EXO_EPERM);
    (void)free_page_owned(theirs, DEMO_OTHER_LIBOS);

    // 3. Nor is the kernel's own memory, however the caller came by the
    //    address: the LibOS window starts above the identity map.
    int64_t r_low = exo_syscall_dispatch(EXO_SYS_PAGE_MAP, 0x200000ULL,
                                         (uint64_t)p, EXO_PAGE_WRITE, 0, 0, 0);
    all &= ownership_check(con, "  kernel address refused (EPERM)         ",
                           r_low == -EXO_EPERM);

    // 4. Unmapping removes the mapping and leaves the page allocated, which is
    //    what makes exo_page_free a separate call.
    int64_t r_unmap = exo_syscall_dispatch(EXO_SYS_PAGE_UNMAP, scratch,
                                           0, 0, 0, 0, 0);
    uint64_t gone = 0;
    all &= ownership_check(con, "  unmap removes only the mapping         ",
                           r_unmap == 0 &&
                           vmm_translate(scratch, &gone, NULL) == VMM_ENOENT &&
                           page_owner((void *)(uintptr_t)p)
                               == syscall_current_context());

    (void)exo_syscall_dispatch(EXO_SYS_PAGE_FREE, (uint64_t)p, 0, 0, 0, 0, 0);
    return all;
}

static void run_ownership_demo(fb_console_t *con, framebuffer_t *fb) {
    klog(con, 0, "Page ownership self-check (SCRUM-152):");
    int all = 1;

    // 1. The LibOS context allocates a page; the kernel stamps it as owner.
    int64_t p = exo_syscall_dispatch(EXO_SYS_PAGE_ALLOC, 0, 0, 0, 0, 0, 0);
    log_prefix(con, 0);
    fbcon_write(con, "  exo_page_alloc -> 0x");
    fbcon_set_color(con, 100, 180, 255, 0, 0, 0);
    fbcon_write_hex64(con, (uint64_t)p);
    fbcon_set_color(con, 220, 220, 220, 0, 0, 0);
    fbcon_write(con, "\n");
    all &= ownership_check(con, "  page handed out (owner = LibOS)         ", p > 0);

    // 2. A kernel-owned page cannot be freed by the LibOS -> -EXO_EPERM.
    void *kp = alloc_page();   // KERNEL-owned
    int64_t r_kern = exo_syscall_dispatch(EXO_SYS_PAGE_FREE,
                                          (uint64_t)(uintptr_t)kp, 0, 0, 0, 0, 0);
    all &= ownership_check(con, "  free of kernel page rejected (EPERM)    ",
                           r_kern == -EXO_EPERM);
    free_page(kp);             // kernel reclaims its own page

    // 3. The owner may free its own page -> 0.
    int64_t r_ok = exo_syscall_dispatch(EXO_SYS_PAGE_FREE, (uint64_t)p, 0, 0, 0, 0, 0);
    all &= ownership_check(con, "  owner frees its own page                ",
                           r_ok == 0);

    // 4. Freeing it again is a double free -> -EXO_EINVAL.
    int64_t r_dbl = exo_syscall_dispatch(EXO_SYS_PAGE_FREE, (uint64_t)p, 0, 0, 0, 0, 0);
    all &= ownership_check(con, "  double free rejected (EINVAL)           ",
                           r_dbl == -EXO_EINVAL);

    all &= run_fb_binding_demo(con);
    all &= run_page_map_demo(con);
    all &= run_revocation_demo(con);

    // Visible status swatch, top-right corner: green = enforced, red = broken.
    const uint32_t sw = 24;
    if (fb->width > sw + 8) {
        if (all) fb_fill_rect(fb, fb->width - sw - 8, 8, sw, sw, 80, 210, 80);
        else     fb_fill_rect(fb, fb->width - sw - 8, 8, sw, sw, 230, 50, 50);
    }

    /* Say it on serial as well as on screen.  The self-checks are the only
     * thing that exercises these syscalls on a *normal* boot -- the KUnit suite
     * runs in a TESTING build with different page tables -- and a verdict that
     * exists only as pixels cannot be checked by CI, by a script, or by anyone
     * who is not looking at the screen at the time. */
    serial_print(all ? "ownership self-check: ENFORCED\n"
                     : "ownership self-check: BROKEN\n");
    serial_flush();

    log_prefix(con, 0);
    fbcon_write(con, "Resource ownership enforcement: ");
    if (all) {
        fbcon_set_color(con, 80, 210, 80, 0, 0, 0);
        fbcon_write(con, "ENFORCED\n");
    } else {
        fbcon_set_color(con, 230, 50, 50, 0, 0, 0);
        fbcon_write(con, "BROKEN\n");
    }
    fbcon_set_color(con, 220, 220, 220, 0, 0, 0);
}

// ── Kernel entry ──────────────────────────────────────────────────────────

void kernel_main(void *mb2_info_ptr) {
    serial_init();

    const struct mb2_info *mb = (const struct mb2_info *)mb2_info_ptr;
    serial_print("Kernel Booted (x86_64)\n");

    // ── Framebuffer discovery (serial diagnostics only; no console yet) ──
    const struct mb2_tag *fb_tag = mb2_find_tag(mb, MB2_TAG_FRAMEBUFFER);

    if (fb_tag) {
        const struct mb2_tag_framebuffer *fbi =
            (const struct mb2_tag_framebuffer *)fb_tag;

        serial_print("Framebuffer: addr=0x");
        serial_print_hex64(fbi->addr);
        serial_print(" ");
        serial_print_u32(fbi->width);
        serial_print("x");
        serial_print_u32(fbi->height);
        serial_print(" ");
        serial_print_u32(fbi->bpp);
        serial_print("bpp\n");
    }

    // ── Memory map ──────────────────────────────────────────────────────
    mmap_init(mb);

    // ── Memory subsystem ────────────────────────────────────────────────
    memory_init();

    // ── IDT (SCRUM-17) ──────────────────────────────────────────────────
    // As early as serial allows, and well ahead of the TESTING branch.  The
    // point is the vector-14 handler: everything below this line -- the PMM,
    // the page tables, the syscall init, the test suite itself -- gets a
    // serial diagnostic on a page fault instead of a silent loop back into
    // the faulting instruction.
    //
    // Safe this early for exactly one reason: IF is clear.  The CPU comes out
    // of boot.s with interrupts disabled and nothing calls `sti` until after
    // pic_remap() far below, so no hardware IRQ can arrive in between.  Do
    // NOT rely on the PIC being masked here -- the BIOS typically leaves
    // IRQ0/IRQ1 unmasked, and until pic_remap() they still land on vectors
    // 8-15, where vector 8's error_stub would pop an error code the PIC never
    // pushed.  Anything added between here and pic_remap() must leave IF
    // alone.
    idt_init();
    // ── Page allocator (SCRUM-7) ───────────────────────────────────────
    page_alloc_init(mb);

    // ── Kernel page tables (SCRUM-15) ───────────────────────────────────
    // Replaces boot.s's blanket 4 GB identity map with tables built from PMM
    // pages that describe only what the kernel has: low memory, the kernel
    // image and bump pool, usable RAM (WAD module included) and the
    // framebuffer aperture.  Needs the PMM, so it sits after page_alloc_init;
    // ahead of the TESTING branch because run_tests() -- the ring-3 probe
    // included -- then runs against this map rather than the boot one.
    //
    // On failure CR3 is untouched and the boot map stays live, so the kernel
    // keeps running (degraded, still on the boot map) and says so.
    if (vmm_init(mb, (const struct mb2_tag_framebuffer *)fb_tag) != VMM_OK) {
        serial_print("WARN: vmm_init failed; continuing on the boot map\n");
    }

    // ── Syscall entry (SCRUM-32) ────────────────────────────────────────
    // Programs EFER.SCE/STAR/LSTAR/FMASK so the `syscall` instruction has a
    // landing site.  Ahead of the TESTING branch because the ring-3 test
    // needs it, and harmless on a normal boot: no handler is registered yet,
    // so every syscall number answers -EXO_ENOSYS until SCRUM-33.
    syscall_init();

    // ── Memory syscalls (SCRUM-34) ──────────────────────────────────────
    // Binds exo_page_alloc (#0) and exo_page_free (#1) to the dispatcher.
    // After page_alloc_init + syscall_init, and ahead of the TESTING branch
    // so the handlers are registered for both a normal boot and the tests.
    syscall_mem_init();

    // ── Framebuffer secure binding (SCRUM-154) ──────────────────────────
    // Publishes the framebuffer's geometry to the binding table and binds
    // exo_fb_acquire (#4).  Same placement rule as the memory syscalls: after
    // syscall_init, ahead of the TESTING branch.  fb_tag may be NULL, in which
    // case acquire reports -EXO_ENODEV rather than -EXO_ENOSYS.
    syscall_fb_init((const struct mb2_tag_framebuffer *)fb_tag);

#ifdef TESTING
    serial_flush();
    qemu_exit((uint32_t)run_tests());
#endif

    // ── Framebuffer init ────────────────────────────────────────────────
    framebuffer_t fb;
    fb_console_t con;
    int have_fb = 0;

    if (fb_tag) {
        const struct mb2_tag_framebuffer *fbi =
            (const struct mb2_tag_framebuffer *)fb_tag;

        if (fb_init_bgrx8888(&fb,
                              (uintptr_t)fbi->addr,
                              fbi->pitch,
                              fbi->width,
                              fbi->height,
                              fbi->bpp) &&
            fbcon_init(&con, &fb)) {
            have_fb = 1;
            fb_clear(&fb, 0, 0, 0);
        }
    }

    if (!have_fb) {
        serial_print("FATAL: no framebuffer\n");
        serial_flush();
        for (;;);
    }

    // ── Boot banner ─────────────────────────────────────────────────────
    fbcon_set_color(&con, 100, 220, 255, 0, 0, 0);
    fbcon_write(&con, "ExoDoom 0.1.0 (x86_64)\n");
    fbcon_set_color(&con, 60, 60, 60, 0, 0, 0);
    fbcon_write(&con, "-----------------------------------------------"
                      "--------------------------------\n");

    klog(&con, 0, "Booted via GRUB Multiboot 2");

    log_prefix(&con, 0);
    fbcon_write(&con, "Framebuffer: ");
    fbcon_set_color(&con, 100, 180, 255, 0, 0, 0);
    {
        const struct mb2_tag_framebuffer *fbi =
            (const struct mb2_tag_framebuffer *)fb_tag;
        fbcon_write_u32(&con, fbi->width);
        fbcon_write(&con, "x");
        fbcon_write_u32(&con, fbi->height);
        fbcon_write(&con, " ");
        fbcon_write_u32(&con, fbi->bpp);
        fbcon_write(&con, "bpp @ 0x");
        fbcon_write_hex64(&con, fbi->addr);
    }
    fbcon_set_color(&con, 220, 220, 220, 0, 0, 0);
    fbcon_write(&con, "\n");

    print_mmap(&con);

    log_prefix(&con, 0);
    fbcon_write(&con, "Kernel allocator base: 0x");
    fbcon_set_color(&con, 100, 180, 255, 0, 0, 0);
    fbcon_write_hex32(&con, (uint32_t)memory_base_address());
    fbcon_set_color(&con, 220, 220, 220, 0, 0, 0);
    fbcon_write(&con, "\n");

    // ── Kernel page tables (SCRUM-15) ───────────────────────────────────
    log_prefix(&con, 0);
    if (vmm_is_active()) {
        fbcon_write(&con, "Kernel page tables: PML4 @ 0x");
        fbcon_set_color(&con, 100, 180, 255, 0, 0, 0);
        fbcon_write_hex64(&con, vmm_kernel_pml4());
        fbcon_set_color(&con, 220, 220, 220, 0, 0, 0);
        fbcon_write(&con, " (");
        fbcon_write_u32(&con, vmm_table_pages());
        fbcon_write(&con, " table pages from the PMM)\n");
    } else {
        fbcon_set_color(&con, 230, 50, 50, 0, 0, 0);
        fbcon_write(&con, "Kernel page tables: FAILED - running on the boot map\n");
        fbcon_set_color(&con, 220, 220, 220, 0, 0, 0);
    }

    // ── Page ownership self-check (SCRUM-152) ───────────────────────────────
    fbcon_write(&con, "\n");
    run_ownership_demo(&con, &fb);
    fbcon_write(&con, "\n");

    // ── IDT / PIC / PIT / PS2 ─────────────────────────────────────���────
    // The IDT is already loaded -- idt_init() runs far above, ahead of the
    // TESTING branch, so a fault anywhere in early boot lands in the page
    // fault handler rather than looping (SCRUM-17).
    klog(&con, 0, "IDT initialized (256 entries)");

    pic_remap();
    klog(&con, 0, "PIC remapped (IRQs -> vectors 0x20-0x2F)");

    idt_set_gate(32, (uintptr_t)irq0_stub);
    pit_init(1000);
    klog(&con, 0, "PIT initialized at 1000 Hz (IRQ0 -> vector 0x20)");

    idt_set_gate(33, (uintptr_t)irq1_stub);
    kbd_init();
    klog(&con, 0, "PS/2 keyboard initialized (IRQ1 -> vector 0x21)");

    __asm__ volatile ("sti");
    klog(&con, 0, "Interrupts enabled (STI)");

    // ── Timer demo ──────────────────────────────────────────────────────
    fbcon_write(&con, "\n");
    klog(&con, 0, "Starting timer demo...");
    fbcon_write(&con, "\n");

    for (int tick = 1; tick <= 9; tick++) {
        kernel_sleep_ms(1000);
        uint32_t ms = kernel_get_ticks_ms();
        log_prefix(&con, ms);
        fbcon_write(&con, "uptime: ");
        fbcon_set_color(&con, 100, 220, 255, 0, 0, 0);
        fbcon_write_u32(&con, ms);
        fbcon_set_color(&con, 220, 220, 220, 0, 0, 0);
        fbcon_write(&con, " ms\n");
    }

    fbcon_write(&con, "\n");
    klog(&con, kernel_get_ticks_ms(), "Timer demo complete.");

    // ── Keyboard event loop ─────────────────────────────────────────────
    // IRQ1 only decodes scancodes and queues events; draining and printing
    // them happens here, out of interrupt context.
    klog(&con, kernel_get_ticks_ms(),
         "Keyboard ready - key events are logged to serial.");

    for (;;) {
        kbd_service();

        // Check the queue with interrupts off, so an IRQ1 landing between the
        // service call and the hlt cannot leave its events sitting unread
        // until some unrelated interrupt happens to wake the CPU. Today the
        // 1000 Hz PIT bounds that to ~1 ms, but it becomes a real stall if the
        // tick rate drops or the timer stops.
        //
        // "sti; hlt" is the safe pairing: sti leaves interrupts blocked for
        // one more instruction, so the hlt is executed before any IRQ is
        // recognised and a wakeup arriving in that window cannot be lost.
        //
        // The "memory" clobbers make the barrier explicit rather than relying
        // on kbd_pending() being an opaque cross-TU call: without them a build
        // that can see through it (LTO, or moving it inline) would be free to
        // hoist the queue read out of the critical section and reintroduce the
        // race this block closes.
        __asm__ volatile ("cli" ::: "memory");

        if (kbd_pending()) {
            __asm__ volatile ("sti" ::: "memory");
            continue;
        }

        __asm__ volatile ("sti; hlt" ::: "memory");
    }
}
