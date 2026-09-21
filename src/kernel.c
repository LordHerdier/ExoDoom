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
#include "syscall_stat.h"
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

static void fbcon_write_u32(fb_console_t *con, uint32_t val) {
    if (val == 0) { fbcon_write(con, "0"); return; }
    char buf[11];
    buf[10] = '\0';
    int i = 10;
    while (val > 0) { buf[--i] = '0' + (val % 10); val /= 10; }
    fbcon_write(con, &buf[i]);
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

// ── Boot sanity check (SCRUM-191) ──────────────────────────────────────────
//
// A gate, not a report: proves paging is real and the syscall gate + memory
// subsystem actually work end to end before trusting the shell to them, and
// halts loudly instead of letting a broken boot limp forward into an
// unusable shell. Reuses the same exo_page_alloc -> exo_page_map ->
// exo_page_unmap -> exo_page_free round trip the old ownership/page-map
// self-checks (SCRUM-152/-35) proved this with, trimmed to the happy-path
// assertions only -- the negative-path checks those covered (foreign-page
// rejection, kernel-address rejection, double-free rejection, framebuffer
// binding, revocation) are enforcement tests that belong to KUnit/CI, not a
// check that has to run on every boot. The framebuffer itself is not
// re-checked here: the caller already halts above if `have_fb` is false, so
// by the time this runs it is known good.
static void boot_sanity_check(fb_console_t *con) {
    int ok = vmm_is_active();

    if (ok) {
        const uint64_t scratch = EXO_USER_VA_BASE + 0x30000000ULL;
        int64_t p = exo_syscall_dispatch(EXO_SYS_PAGE_ALLOC, 0, 0, 0, 0, 0, 0);
        int64_t r_map = exo_syscall_dispatch(EXO_SYS_PAGE_MAP, scratch,
                                             (uint64_t)p,
                                             EXO_PAGE_READ | EXO_PAGE_WRITE,
                                             0, 0, 0);
        int64_t r_unmap = exo_syscall_dispatch(EXO_SYS_PAGE_UNMAP, scratch,
                                               0, 0, 0, 0, 0);
        int64_t r_free = exo_syscall_dispatch(EXO_SYS_PAGE_FREE, (uint64_t)p,
                                              0, 0, 0, 0, 0);
        ok = p > 0 && r_map == 0 && r_unmap == 0 && r_free == 0;
    }

    if (!ok) {
        klog(con, 0, "FATAL: boot sanity check failed");
        serial_print("FATAL: boot sanity check failed "
                     "(paging or syscall gate not live)\n");
        serial_flush();
        for (;;) {
            __asm__ volatile ("cli; hlt");
        }
    }

    klog(con, 0, "Boot sanity check: OK (paging, syscall gate, "
                "memory subsystem verified)");
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

    // ── Introspection syscalls (SCRUM-113) ──────────────────────────────
    // Binds exo_memstat (#25) and exo_pslist (#26), backing the shell's
    // `memstat`/`pslist` commands. Same placement rule as the syscalls
    // above: after syscall_init, ahead of the TESTING branch, so both are
    // exercisable from the ring-3 test harness too.
    syscall_stat_init();

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
    // Binds exo_launch_wad_viewer (#21): stages the WAD module read-only
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

    // ── Boot sanity check (SCRUM-191) ────────────────────────────────────
    // Gates the rest of boot: halts with a clear reason on serial + screen
    // rather than continuing into a shell that can't actually use memory,
    // paging, or the syscall gate.
    boot_sanity_check(&con);

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
