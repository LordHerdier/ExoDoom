#include <stdint.h>
#include "multiboot2.h"
#include "serial.h"
#include "memory.h"
#include "mmap.h"
#include "page_alloc.h"
#include "syscall_exit.h"

#include "idt.h"
#include "tss.h"
#include "pic.h"
#include "pit.h"
#include "ps2.h"
#include "sleep.h"
#include "fb.h"
#include "fb_console.h"
#include "syscall.h"
#include "syscall_mem.h"
#include "syscall_fb.h"
#include "syscall_serial.h"
#include "syscall_kbd.h"
#include "syscall_pit.h"
#include "syscall_yield.h"
#include "syscall_launch.h"
#include "fb_binding.h"
#include "fb_shadow.h"
#include "revoke.h"
#include "vmm.h"
#include "exo_syscall.h"
#include "libos_launch.h"
#include "context.h"
#include "shell/shell_layout.h"

extern void irq0_stub();
extern void irq1_stub();
extern void kbd_init();

/* Embedded shell LibOS code/data blobs -- produced at build time by
 * build_ring3_link_target's objcopy step (docker/scripts/build.sh, SCRUM-110)
 * from src/shell/shell_main.c + src/fb.c + src/fb_console.c + src/libos_fb.c,
 * linked and compiled without -DEXO_KERNEL at LIBOS_LAUNCH_CODE_VADDR/
 * _DATA_VADDR. SHELL_BSS_LEN comes from the same step's generated
 * src/shell/shell_layout.h, mirroring exactly how
 * tests/kernel/test_libc_shim_probe_k.c consumes its own blobs. */
extern const uint8_t _binary_shell_code_bin_start[];
extern const uint8_t _binary_shell_code_bin_end[];
extern const uint8_t _binary_shell_data_bin_start[];
extern const uint8_t _binary_shell_data_bin_end[];

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
    int all = 1;

    klog(con, 0, "Framebuffer binding self-check (SCRUM-154):");

    // exo_fb_acquire's info_out is bounds-checked against the LibOS window
    // (SCRUM-54), same as exo_serial_write's buf — a kernel-stack local no
    // longer qualifies, so this demo maps a scratch page the same way
    // run_page_map_demo() does, at a VA of its own well clear of that one's.
    const uint64_t scratch = EXO_USER_VA_BASE + 0x38000000ULL;
    int64_t scratch_p = exo_syscall_dispatch(EXO_SYS_PAGE_ALLOC, 0, 0, 0, 0, 0, 0);
    int64_t scratch_map = exo_syscall_dispatch(EXO_SYS_PAGE_MAP, scratch,
                                               (uint64_t)scratch_p,
                                               EXO_PAGE_WRITE, 0, 0, 0);
    if (scratch_p <= 0 || scratch_map != 0) {
        klog(con, 0, "  scratch mapping for info_out failed, skipping\n");
        return 0;
    }

    exo_fb_info_t *info = (exo_fb_info_t *)(uintptr_t)scratch;
    *info = (exo_fb_info_t){ 0, 0, 0, 0, 0, { 0, 0, 0 } };

    // 1. Acquire hands the calling context its own private virtual
    //    framebuffer (SCRUM-112) -- it no longer touches fb_binding at all,
    //    so fb_binding_owner() stays unheld throughout this demo.
    int64_t r_acq = exo_syscall_dispatch(EXO_SYS_FB_ACQUIRE,
                                         (uint64_t)(uintptr_t)info,
                                         0, 0, 0, 0, 0);
    log_prefix(con, 0);
    fbcon_write(con, "  exo_fb_acquire -> phys 0x");
    fbcon_set_color(con, 100, 180, 255, 0, 0, 0);
    fbcon_write_hex64(con, info->phys_addr);
    fbcon_set_color(con, 220, 220, 220, 0, 0, 0);
    fbcon_write(con, "\n");
    all &= ownership_check(con, "  private FB buffer allocated             ",
                           r_acq == 0 &&
                           fb_binding_owner() == PAGE_OWNER_FREE);

    // 2. The real framebuffer's own physical range is still guarded by
    //    fb_binding_check_map() -- nobody holds that binding anymore (see
    //    1.), so it denies everyone now, this context included. What
    //    SCRUM-153 originally proved here (an owner may map its own FB
    //    pages) is just ordinary page ownership for the private buffer, and
    //    run_page_map_demo() below already covers that generically.
    all &= ownership_check(con, "  real FB range still unmappable          ",
                           fb_binding_geometry() != NULL &&
                           fb_binding_check_map(
                               fb_binding_geometry()->phys_addr,
                               syscall_current_context())
                           == FB_MAP_DENY);

    // 3. A second, distinct context also succeeds, with its own distinct
    //    buffer -- SCRUM-112 replaced the single exclusive binding with a
    //    private buffer per caller, so there is no exclusivity left to
    //    demonstrate here the way there used to be.
    exo_fb_info_t other_info;
    int r_other = fb_shadow_acquire(DEMO_OTHER_LIBOS, &other_info);
    all &= ownership_check(con, "  second acquirer also succeeds           ",
                           r_other == FB_SHADOW_OK &&
                           other_info.phys_addr != info->phys_addr);

    // 4. Reclamation (what SCRUM-155's exo_exit does) frees both buffers
    //    again, so the compositor (src/fb_compositor.c) has nothing stale
    //    left to composite once this demo is done -- unlike
    //    fb_binding_release(), this now has a real, visible effect, so it
    //    is not optional cleanup the way the scratch-page unmap below
    //    always was.
    fb_shadow_release(syscall_current_context());
    fb_shadow_release(DEMO_OTHER_LIBOS);
    (void)reclaim_pages_owned(syscall_current_context());
    (void)reclaim_pages_owned(DEMO_OTHER_LIBOS);

    (void)exo_syscall_dispatch(EXO_SYS_PAGE_UNMAP, scratch, 0, 0, 0, 0, 0);
    (void)exo_syscall_dispatch(EXO_SYS_PAGE_FREE, (uint64_t)scratch_p,
                               0, 0, 0, 0, 0);

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

    // exo_page_map installs `scratch` in the caller's *registered* address
    // space (src/syscall_mem.c's caller_pml4(), SCRUM-48) — today that is
    // vmm_kernel_pml4() itself (kernel_main binds it that way until SCRUM-47
    // gives the LibOS a real one), but this demo runs at ring 0 without ever
    // switching CR3, so it must resolve the same root the syscall used
    // rather than assume it is whichever tree happens to be loaded.
    uint64_t root_phys = vmm_address_space_for(syscall_current_context());
    uint64_t *root = (uint64_t *)(uintptr_t)root_phys;

    // 1. A page the caller owns can be mapped where the caller asks, and the
    //    mapping is real: it resolves to the right physical frame, and a
    //    write through that frame's identity-mapped address (always safe —
    //    the kernel's own map covers every page it owns, regardless of which
    //    root exo_page_map used) is visible there afterwards.
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
        *(volatile uint64_t *)(uintptr_t)p = 0x5CA1AB1E5CA1AB1EULL;

    uint64_t resolved = 0;
    all &= ownership_check(con, "  owned page mapped and writable         ",
                           p > 0 && r_map == 0 && root != NULL &&
                           vmm_translate_in(root, scratch, &resolved, NULL) == VMM_OK &&
                           resolved == (uint64_t)p &&
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
                           r_unmap == 0 && root != NULL &&
                           vmm_translate_in(root, scratch, &gone, NULL) == VMM_ENOENT &&
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

// ── PIC/PIT boot narration (SCRUM-172) ──────────────────────────────────────
//
// pic_remap()/idt_set_gate(32)/pit_init() themselves run early in
// kernel_main, ahead of the TESTING branch (see that call site) -- long
// before the framebuffer console they'd narrate to exists. This function is
// the one place that narration text lives; kernel_main calls it once, later,
// once `con` exists, instead of restating the same two lines inline at that
// distant call site where nothing but a comment would keep them in sync with
// what actually ran.
static void log_pic_pit_ready(fb_console_t *con) {
    klog(con, 0, "PIC remapped (IRQs -> vectors 0x20-0x2F)");
    klog(con, 0, "PIT initialized at 1000 Hz (IRQ0 -> vector 0x20)");
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
    // of boot.s with interrupts disabled and nothing calls `sti` before
    // pic_remap() a short way below (SCRUM-172 moved it up from the
    // normal-boot tail), so no hardware IRQ can arrive in between.  Do
    // NOT rely on the PIC being masked here -- the BIOS typically leaves
    // IRQ0/IRQ1 unmasked, and until pic_remap() they still land on vectors
    // 8-15, where vector 8's error_stub would pop an error code the PIC never
    // pushed.  Anything added between here and pic_remap() must leave IF
    // alone.
    idt_init();

    // ── TSS (SCRUM-46) ───────────────────────────────────────────────────
    // Every gate idt_init() just installed has IST=0, so a CPL 3 -> CPL 0
    // exception loads its stack from TSS.RSP0. Without a TSS loaded, TR is
    // null and that load itself faults -- straight to #GP, then #DF, then a
    // triple fault, before any handler runs. Depends on nothing but static
    // storage, so it goes in immediately after idt_init() and, like it,
    // ahead of the TESTING branch: the ring-3 fault tests need it, and nothing
    // reaches ring 3 to exercise it before that (SCRUM-47) on a normal boot.
    tss_init();

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
    } else {
        // ── PAGE_OWNER_LIBOS placeholder bind ────────────────────────────
        // Kept even after SCRUM-178 gave the shell a real context_create()'d
        // row (see the shell-launch tail below): this runs on every boot,
        // TESTING included, and TESTING's run_tests() exits before the shell
        // launch code ever runs, so this is the only thing that keeps
        // PAGE_OWNER_LIBOS bound during a test run.
        // tests/kernel/test_context_k.c's test_create_never_steals_libos_
        // binding() depends on this exact binding existing up front -- it is
        // the regression test for context_create() otherwise handing this id
        // straight back out to the first caller. Because this binding exists
        // first, context_create()'s id search in the shell-launch tail below
        // skips PAGE_OWNER_LIBOS and hands the shell the next id instead
        // (harmless: nothing depends on the shell's id being PAGE_OWNER_LIBOS
        // specifically any more, now that it is a real context_t row).
        if (vmm_bind_address_space(PAGE_OWNER_LIBOS, vmm_kernel_pml4()) != VMM_OK) {
            serial_print("WARN: vmm_bind_address_space failed; "
                         "exo_page_map/-unmap will report -EXO_EINVAL\n");
        }
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

    // ── Exit syscall (SCRUM-155) ────────────────────────────────────────
    // Binds exo_exit (#20), which reclaims every page and the framebuffer
    // binding the terminating LibOS context holds.  Same placement rule as
    // the other syscalls: after syscall_mem_init()/syscall_fb_init(), ahead
    // of the TESTING branch.
    syscall_exit_init();

    // ── Serial syscall (SCRUM-50) ────────────────────────────────────────
    // Binds exo_serial_write (#8), the printf/fprintf shim's backend.  Same
    // placement rule as the memory and framebuffer syscalls: after
    // syscall_init, ahead of the TESTING branch.
    syscall_serial_init();

    // ── Keyboard syscall (SCRUM-39) ───────────────────────────────────────
    // Binds exo_kbd_poll (#6) to the kernel's existing keyboard ring
    // (src/ps2.c/h, src/kbd_ring.c/h) -- the same ring the ring-0 automap
    // demo already drains directly, now also reachable from ring 3. Same
    // placement rule as the other syscalls: after syscall_init, ahead of
    // the TESTING branch. Harmless before kbd_init() runs (below, in the
    // normal-boot tail): the ring is simply empty until then, and nothing
    // calls this handler during a TESTING build.
    syscall_kbd_init();

    // ── PIC / PIT (SCRUM-172) ────────────────────────────────────────────
    // Moved ahead of the TESTING branch, same reasoning as tss_init() for
    // SCRUM-46: exo_get_ticks (#5) needs a live, advancing tick count to
    // prove itself from ring 3, and pit_init()/IRQ0 previously ran only in
    // the normal-boot tail below, well after a TESTING build has already
    // exited. IRQ1/keyboard wiring (idt_set_gate(33), kbd_init(),
    // pic_unmask_irq(1)) stays in the tail -- nothing under TESTING touches
    // the keyboard, and pic_remap() now leaves IRQ1 masked at the PIC
    // precisely so a stray one arriving before kbd_init() runs cannot reach
    // idt_init()'s default_stub, whose bare iretq sends no EOI and would
    // wedge IRQ1's in-service bit for good (see src/pic.c).
    //
    // log_pic_pit_ready() (defined just above, ahead of kernel_main) is
    // where this gets narrated to the console -- later in this same
    // function, once one exists.
    pic_remap();
    idt_set_gate(32, (uintptr_t)irq0_stub);
    pit_init(1000);

    // ── Timer syscall (SCRUM-172) ────────────────────────────────────────
    // Binds exo_get_ticks (#5). After pit_init() -- the handler reports
    // kernel_get_ticks_ms(), which stays at 0 without it -- and, like the
    // other syscall *_init()s, ahead of the TESTING branch.
    syscall_pit_init();

    // ── Yield syscall (SCRUM-109) ────────────────────────────────────────
    // Binds exo_yield (#19) to context_switch_request() via the round-robin
    // policy in context_next_ready() -- needs no other subsystem init, so
    // placement here just follows the "after syscall_init(), ahead of the
    // TESTING branch" rule every other syscall *_init() follows.
    syscall_yield_init();

    // ── WAD viewer launch syscall (SCRUM-178) ────────────────────────────
    // Binds exo_launch (#21): stages the WAD module read-only
    // into a fresh LibOS address space and context_switch_request()s to it,
    // so the shell's `wadview` command can invoke it as an ordinary program.
    // Same placement rule as every other syscall *_init(): after
    // syscall_init(), ahead of the TESTING branch.
    syscall_launch_init();

#ifdef TESTING
    // No blanket `sti` here: several suites (fault, tss, libos_launch,
    // libos_main) drive a real ring-3 fault on purpose with RFLAGS.IF
    // hardcoded clear (tests/kernel/ring3_probe.s, src/libos_enter.s), and
    // their hook-driven resume (src/fault.c's test_hook) `iretq`s with that
    // same saved RFLAGS -- so IF ends up clear again after any of them runs,
    // no matter what it was set to here. A single early `sti` would only be
    // true until the first such suite, which is worse than not claiming it at
    // all. The one test that needs ticks to actually advance
    // (tests/kernel/test_syscall_pit_k.c) enables interrupts for just its own
    // wait instead.
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

    // ── IDT / PIC / PIT / PS2 ───────────────────────────────────────────
    // The IDT is already loaded -- idt_init() runs far above, ahead of the
    // TESTING branch, so a fault anywhere in early boot lands in the page
    // fault handler rather than looping (SCRUM-17).
    klog(&con, 0, "IDT initialized (256 entries)");

    // pic_remap()/idt_set_gate(32)/pit_init() already ran above, ahead of
    // the TESTING branch (SCRUM-172) -- see that call site's own comment.
    log_pic_pit_ready(&con);

    idt_set_gate(33, (uintptr_t)irq1_stub);
    kbd_init();
    // Only now is IRQ1 unmasked at the PIC (src/pic.c) -- the vector is
    // wired to irq1_stub and kbd_init() has drained any stale byte, so a
    // keyboard interrupt landing right after this has somewhere real to go.
    pic_unmask_irq(1);
    klog(&con, 0, "PS/2 keyboard initialized (IRQ1 -> vector 0x21)");

    __asm__ volatile ("sti");
    klog(&con, 0, "Interrupts enabled (STI)");

    // ── Timer demo ──────────────────────────────────────────────────────
    fbcon_write(&con, "\n");
    klog(&con, 0, "Starting timer demo...");
    fbcon_write(&con, "\n");

    uint32_t timer_demo_start = kernel_get_ticks_ms();
    for (int tick = 1; tick <= 9; tick++) {
        kernel_sleep_until_ms(timer_demo_start + (uint32_t)tick * 1000);
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

    // ── Shell LibOS launch (SCRUM-110, refactored by SCRUM-178) ─────────
    // The first real LibOS this kernel launches on a normal boot, replacing
    // the kernel-mode keyboard loop that used to sit here. Goes through the
    // real context table (context_create()/context_prime()/
    // context_set_current(), src/context.h, SCRUM-107/108) rather than the
    // one-off PAGE_OWNER_LIBOS direct vmm bind SCRUM-110 originally used --
    // SCRUM-178 needs the shell to have a real context_t row so
    // context_switch_request()/exo_yield() can later switch to and from a
    // second LibOS it launches (see src/syscall_launch.c). context_create()
    // is called with the still-live vmm_kernel_pml4() binding as a
    // placeholder pml4 purely to reserve an id and a table slot;
    // libos_build_image() below rebinds that same id's vmm entry to the
    // shell's own, real address space in place (vmm_bind_address_space()
    // allows a rebind), exactly as the old direct-bind version did for
    // PAGE_OWNER_LIBOS. PAGE_OWNER_LIBOS itself is still pre-bound (see the
    // placeholder bind near vmm_init() above -- kept for
    // test_create_never_steals_libos_binding()'s sake), so
    // context_create()'s id search skips it and hands the shell the next id
    // instead; nothing depends on the shell's id being PAGE_OWNER_LIBOS
    // specifically any more, now that it is a real context_t row reached
    // through context_current() rather than a hardcoded constant.
    klog(&con, kernel_get_ticks_ms(), "Launching shell LibOS...");

    page_owner_t shell_id;
    int shell_create_rc = context_create(vmm_kernel_pml4(), &shell_id);
    if (shell_create_rc != CONTEXT_OK) {
        klog(&con, kernel_get_ticks_ms(),
             "FATAL: shell context_create failed");
        serial_print("FATAL: context_create failed for shell LibOS\n");
        serial_flush();
        for (;;) {
            __asm__ volatile ("cli; hlt");
        }
    }

    size_t shell_code_len = (size_t)(_binary_shell_code_bin_end -
                                     _binary_shell_code_bin_start);
    size_t shell_data_len = (size_t)(_binary_shell_data_bin_end -
                                     _binary_shell_data_bin_start);

    /* static, not a stack local: libos_image_t carries one uint64_t per
     * mappable page, so at SCRUM-66's 192/192/16 page caps it is a little
     * over 3 KiB -- a fifth of the 16 KiB boot stack (src/boot.s) to spend
     * on one variable that was a few hundred bytes when this was written.
     * kernel_main runs once and never reenters, so static costs nothing.
     * src/syscall_launch.c's handlers do the same, for the same reason. */
    static libos_image_t shell_img;
    int shell_build_rc = libos_build_image(shell_id,
                                           _binary_shell_code_bin_start,
                                           shell_code_len,
                                           _binary_shell_data_bin_start,
                                           shell_data_len,
                                           SHELL_BSS_LEN,
                                           &shell_img);
    if (shell_build_rc != VMM_OK) {
        klog(&con, kernel_get_ticks_ms(),
             "FATAL: shell LibOS image build failed");
        serial_print("FATAL: libos_build_image failed for shell LibOS\n");
        serial_flush();
        for (;;) {
            __asm__ volatile ("cli; hlt");
        }
    }

    // Seed the shell's saved-register area with this launch's iretq frame
    // and declare it the running context -- context_set_current()'s own
    // documented use case (src/context.h) is exactly this: the first
    // dispatch of a context's life, with no "previous" context to switch
    // away from. context_set_state() then marks it RUNNING for bookkeeping
    // accuracy; context_switch_request() itself only requires the row to
    // exist, not any particular prior state.
    context_prime(shell_id, shell_img.entry_vaddr, shell_img.stack_top_vaddr);
    context_set_current(shell_id);
    context_set_state(shell_id, CONTEXT_STATE_RUNNING);

    // vmm_switch_address_space() rather than folding this into
    // libos_enter_irq(): same reasoning as libos_enter_irq()'s own comment
    // (src/libos_launch.h) -- entry_vaddr/stack_top_vaddr only resolve once
    // this address space is actually loaded in CR3.
    if (vmm_switch_address_space(shell_img.pml4_phys) != VMM_OK) {
        klog(&con, kernel_get_ticks_ms(),
             "FATAL: could not switch to shell LibOS address space");
        serial_print("FATAL: vmm_switch_address_space failed for shell LibOS\n");
        serial_flush();
        for (;;) {
            __asm__ volatile ("cli; hlt");
        }
    }

    // Does not return -- there is no `ret` on the other side of an `iretq`
    // to CPL 3 (src/libos_launch.h's own comment on libos_enter()/
    // libos_enter_irq()). This is legitimately the new tail of kernel_main:
    // the shell LibOS is the rest of this boot.
    libos_enter_irq(shell_img.entry_vaddr, shell_img.stack_top_vaddr);

    // Unreachable.
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}
