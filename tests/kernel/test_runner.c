/*
 * test_runner.c — Top-level test entry point for kernel test builds.
 *
 * Registers all test suites and runs them via KUnit.  Called from
 * kernel_main when the kernel is compiled with -DTESTING.
 *
 * Returns 0 if all tests pass, 1 if any test fails.
 */

#include "kunit.h"

/* Suite registration functions defined in their respective test files. */
void suite_smoke_tests   (CU_pSuite s);
void suite_string_tests  (CU_pSuite s);
void suite_ctype_tests   (CU_pSuite s);
void suite_stdio_tests   (CU_pSuite s);
void suite_stdlib_tests  (CU_pSuite s);
void suite_kbd_ring_tests(CU_pSuite s);
void suite_ps2_decode_tests(CU_pSuite s);
void suite_exo_syscall_tests(CU_pSuite s);
void suite_exo_syscall_kview_tests(CU_pSuite s);
void suite_exo_errno_tests(CU_pSuite s);
void suite_syscall_tests(CU_pSuite s);
void suite_syscall_mem_tests(CU_pSuite s);
void suite_syscall_exit_tests(CU_pSuite s);
void suite_ownership_tests(CU_pSuite s);
void suite_page_alloc_tests(CU_pSuite s);
void suite_memory_isolation_tests(CU_pSuite s);
void suite_fb_binding_tests(CU_pSuite s);
void suite_vmm_tests(CU_pSuite s);
void suite_vmm_fb_wad_tests(CU_pSuite s);
void suite_revoke_tests(CU_pSuite s);
void suite_page_map_tests(CU_pSuite s);
void suite_fault_tests(CU_pSuite s);
void suite_heap_tests(CU_pSuite s);
void suite_heap_stress_tests(CU_pSuite s);
void suite_tss_tests(CU_pSuite s);
void suite_libos_launch_tests(CU_pSuite s);
void suite_syscall_serial_tests(CU_pSuite s);
void suite_syscall_kbd_tests(CU_pSuite s);
void suite_pit_tests(CU_pSuite s);
void suite_speaker_tests(CU_pSuite s);
void suite_syscall_sound_tests(CU_pSuite s);
int syscall_sound_suite_cleanup(void);
void suite_syscall_pit_tests(CU_pSuite s);
void suite_doomgeneric_timer_tests(CU_pSuite s);
void suite_libos_main_tests(CU_pSuite s);
void suite_libos_c_probe_tests(CU_pSuite s);
void suite_libc_shim_probe_tests(CU_pSuite s);
void suite_libos_page_alloc_tests(CU_pSuite s);
void suite_libos_heap_tests(CU_pSuite s);
void suite_libos_heap_stress_tests(CU_pSuite s);
void suite_port_io_fault_tests(CU_pSuite s);
void suite_kernel_mem_fault_tests(CU_pSuite s);
void suite_irq_entry_tests(CU_pSuite s);
void suite_context_tests(CU_pSuite s);
void suite_fb_console_tests(CU_pSuite s);
void suite_context_switch_tests(CU_pSuite s);
void suite_sse_tests(CU_pSuite s);
void suite_syscall_yield_tests(CU_pSuite s);
void suite_shell_libos_tests(CU_pSuite s);
void suite_clock_libos_tests(CU_pSuite s);
void suite_context_launch_rebind_tests(CU_pSuite s);
void suite_fb_shadow_tests(CU_pSuite s);
void suite_fixed_math_tests(CU_pSuite s);
void suite_doom_panic_tests(CU_pSuite s);
void suite_dg_init_tests(CU_pSuite s);
void suite_fpconv_tests(CU_pSuite s);
void suite_libc_gaps_tests(CU_pSuite s);
void suite_syscall_fuzz_tests(CU_pSuite s);
void suite_syscall_bench_tests(CU_pSuite s);
void suite_libos_snake_tests(CU_pSuite s);
void suite_doom_keymap_tests(CU_pSuite s);
void suite_doom_sfx_tone_tests(CU_pSuite s);
void suite_doom_sound_tests(CU_pSuite s);
void suite_libos_doom_tests(CU_pSuite s);
void suite_syscall_stat_tests(CU_pSuite s);
void suite_ata_tests(CU_pSuite s);
void suite_syscall_disk_tests(CU_pSuite s);
void suite_disk_binding_tests(CU_pSuite s);
void suite_pci_tests(CU_pSuite s);
void suite_hda_tests(CU_pSuite s);
void suite_doom_dmx_tests(CU_pSuite s);

/* Same defensive shape as context_suite_cleanup (test_context_k.c): the
 * pslist tests create their own scratch contexts and must not leak a PML4
 * into a later suite if an assertion fails mid-test (SCRUM-113). */
int syscall_stat_suite_cleanup(void);

/* Stops the HDA output stream at suite end, so a tone left running cannot
 * keep raising the controller's INTx line into a later suite (SCRUM-210). */
int suite_hda_cleanup(void);

/* Mounts the real freedoom2 GRUB module into the doom_wad registry for the
 * suite's reference-data tests, and unmounts it afterwards -- that registry is
 * process-global state shared with the dg_init suite (SCRUM-211). */
int suite_doom_dmx_init(void);
int suite_doom_dmx_cleanup(void);

/* Acquires the disk binding for the suite's own context before the round-
 * trip/hardware tests run and releases it afterward (SCRUM-188). */
int syscall_disk_suite_init(void);
int syscall_disk_suite_cleanup(void);

int disk_binding_suite_init(void);
int disk_binding_suite_cleanup(void);

/* Suite init/cleanup for the framebuffer binding suite: it swaps in a
 * synthetic framebuffer geometry and must put the real one back (SCRUM-154). */
int fb_binding_suite_init(void);
int fb_binding_suite_cleanup(void);

/* Same idea for the virtual-framebuffer suite: it swaps in a synthetic
 * geometry and allocates real shadow buffers, and must leave neither behind
 * (SCRUM-112). */
int fb_shadow_suite_init(void);
int fb_shadow_suite_cleanup(void);

/* Same idea for the revocation suite: it borrows the framebuffer and allocates
 * pages under a second context id, and must leave neither behind (SCRUM-156). */
int revoke_suite_init(void);
int revoke_suite_cleanup(void);

/* The page-map suite borrows the framebuffer binding and a second context id
 * to prove what they refuse; it must leave neither behind (SCRUM-35). */
int page_map_suite_cleanup(void);

/* The fault suite installs a TESTING-only page-fault hook; leaving one
 * installed would make a later genuine fault resume into a stale label
 * instead of reporting (SCRUM-17). */
int fault_suite_cleanup(void);

/* The heap stress suite (SCRUM-27) runs its whole 2-pass, 20,000-allocation
 * load in suite init so the tests below can each make one independent claim
 * about the same run.  It allocates nothing that outlives init and so needs
 * no cleanup. */
int heap_stress_suite_init(void);

/* Same idea for the TSS suite: it installs the fault hook and a SYS_ESCAPE
 * handler around its live ring-3 fault test (SCRUM-46). */
int tss_suite_cleanup(void);

/* Same idea for the libos_launch suite: it installs the fault hook, a
 * SYS_LIBOS_RETURN handler, and a real address space around its live
 * ring-3 launch test (SCRUM-47). */
int libos_launch_suite_cleanup(void);

/* Same idea for the libos_main suite: it installs the fault hook, a
 * SYS_LIBOS_RETURN handler, and a real address space around its live
 * ring-3 entry-framework test (SCRUM-50). */
int libos_main_suite_cleanup(void);

/* Same idea for the libos_c_probe suite: it installs the fault hook, a
 * SYS_LIBOS_RETURN handler, and a real address space around its live
 * compiled-ring-3 test (SCRUM-173). */
int libos_c_probe_suite_cleanup(void);

/* Same idea for the libc_shim_probe suite: it installs the fault hook, a
 * SYS_LIBOS_RETURN handler, and a real address space around its live
 * compiled-libc-shim-in-ring-3 test (SCRUM-51). Its init additionally saves
 * PAGE_OWNER_LIBOS's boot-time address-space binding, which cleanup restores
 * -- see test_libc_shim_probe_k.c's own comment on why this suite (alone)
 * has to. */
int libc_shim_probe_suite_init(void);
int libc_shim_probe_suite_cleanup(void);

/* Same idea for the syscall_bench suite (SCRUM-60): it installs the fault
 * hook, a SYS_LIBOS_RETURN handler, and a real address space around each
 * benchmark's live ring-3 launch. Its init additionally saves
 * PAGE_OWNER_LIBOS's boot-time address-space binding for the same reason
 * libc_shim_probe's does -- see test_syscall_bench_k.c's own comment. */
int syscall_bench_suite_init(void);
int syscall_bench_suite_cleanup(void);

/* Same idea for the port_io_fault suite: it installs the fault hook, a
 * SYS_LIBOS_RETURN handler, and a real address space around its live
 * ring-3 port-I/O trap test (SCRUM-56). */
int port_io_fault_suite_cleanup(void);

/* Same idea for the kernel_mem_fault suite: it installs the fault hook, a
 * SYS_LIBOS_RETURN handler, and a real address space around its live
 * ring-3 kernel-memory-fault test (SCRUM-55). */
int kernel_mem_fault_suite_cleanup(void);

/* Same idea for the irq_entry suite: it installs the fault hook, a
 * SYS_LIBOS_RETURN handler, and a real address space around its live
 * ring-3 hardware-interrupt test (SCRUM-170). */
int irq_entry_suite_cleanup(void);

/* The context suite (SCRUM-107) creates real address spaces via
 * vmm_create_address_space() and hands them to context_create(); a failed
 * assertion mid-test could leave one bound with no test left to destroy it,
 * so cleanup sweeps every context this suite's tests could have created. */
int context_suite_cleanup(void);

/* Same idea for the context_switch suite (SCRUM-108), extended with
 * page_reclaim_all(): unlike the context suite above, these contexts have
 * real code/data/stack pages mapped into them, not just a bare PML4. */
int context_switch_suite_cleanup(void);

/* Same idea for the context_launch_rebind suite (SCRUM-178): its contexts
 * are also launched with real code/data/stack pages mapped in. */
int context_launch_rebind_suite_cleanup(void);

/* Same idea for the sse suite (SCRUM-177): its two ring-3 cases each build a
 * real address space and install the fault hook and a SYS_LIBOS_RETURN
 * handler around a live launch, and the second of them leaves irq0_handler's
 * recorded entry RSP set. */
int sse_suite_cleanup(void);

/* The sse suite reads CR0/CR4 in init, before any of its cases executes an
 * SSE instruction, so a kernel built without the enable block fails those
 * cases instead of #UD-ing into an infinite loop. See test_sse_k.c. */
int sse_suite_init(void);

/* Same idea for the syscall_yield suite (SCRUM-109): same real-address-space/
 * real-page shape as context_switch above, driven through the real bound
 * exo_yield handler instead of a test-local stand-in. */
int syscall_yield_suite_cleanup(void);

/* Same idea for the shell_libos suite (SCRUM-110): builds the real shell
 * blob under PAGE_OWNER_LIBOS and must restore its boot-time binding, same
 * reasoning as libc_shim_probe_suite_init/_cleanup above. */
int shell_libos_suite_init(void);
int shell_libos_suite_cleanup(void);

/* Same idea for the clock_libos suite (SCRUM-168): builds the real clock
 * blob under PAGE_OWNER_LIBOS and must restore its boot-time binding, same
 * reasoning as shell_libos_suite_init/_cleanup above. */
int clock_libos_suite_init(void);
int clock_libos_suite_cleanup(void);

/* Same idea for the libos_snake suite (SCRUM-182): builds the real Snake
 * blob under PAGE_OWNER_LIBOS and must restore its boot-time binding, same
 * reasoning as shell_libos_suite_init/_cleanup above. */
int libos_snake_suite_init(void);
int libos_snake_suite_cleanup(void);

/* Same idea again for libos_doom (SCRUM-66): builds the real Doom image
 * from the embedded blobs, so it needs the same save/restore of
 * PAGE_OWNER_LIBOS's address-space binding. */
int libos_doom_suite_init(void);
int libos_doom_suite_cleanup(void);

/* The libos_heap_stress suite (SCRUM-38) runs its whole 2-pass, ~1,000-
 * allocation load in suite init, same reasoning as heap_stress_suite_init
 * above -- kept as its own suite, separate from "libos_heap"'s plain
 * correctness tests, so those run against a pristine allocator instead of
 * one this load has already warmed up. It allocates nothing that outlives
 * init and so needs no cleanup. It must run after "libos_page_alloc": its
 * segment-growth contiguity assumption depends on that suite's last test,
 * test_slot_table_exhaustion (tests/kernel/test_libos_page_alloc_k.c),
 * having reset libos_page_alloc back to a clean, empty state as its final
 * action. */
int libos_heap_stress_suite_init(void);

/* The fb_console suite (SCRUM-162) runs scroll_up_one_row() against a
 * synthetic, RAM-backed framebuffer rather than the real MMIO one -- init
 * allocates the backing page, cleanup frees it. */
int fb_console_suite_init(void);
int fb_console_suite_cleanup(void);

int run_tests(void)
{
    CU_pSuite s;

    CU_initialize_registry();

    s = CU_add_suite("smoke",  NULL, NULL);
    suite_smoke_tests(s);

    s = CU_add_suite("string", NULL, NULL);
    suite_string_tests(s);

    s = CU_add_suite("ctype",  NULL, NULL);
    suite_ctype_tests(s);

    s = CU_add_suite("stdio",  NULL, NULL);
    suite_stdio_tests(s);

    s = CU_add_suite("stdlib", NULL, NULL);
    suite_stdlib_tests(s);

    s = CU_add_suite("kbd_ring", NULL, NULL);
    suite_kbd_ring_tests(s);

    s = CU_add_suite("ps2_decode", NULL, NULL);
    suite_ps2_decode_tests(s);

    s = CU_add_suite("exo_syscall", NULL, NULL);
    suite_exo_syscall_tests(s);

    s = CU_add_suite("exo_syscall_kview", NULL, NULL);
    suite_exo_syscall_kview_tests(s);

    s = CU_add_suite("exo_errno", NULL, NULL);
    suite_exo_errno_tests(s);

    s = CU_add_suite("syscall", NULL, NULL);
    suite_syscall_tests(s);

    s = CU_add_suite("syscall_mem", NULL, NULL);
    suite_syscall_mem_tests(s);

    s = CU_add_suite("syscall_exit", NULL, NULL);
    suite_syscall_exit_tests(s);

    s = CU_add_suite("ownership", NULL, NULL);
    suite_ownership_tests(s);

    s = CU_add_suite("page_alloc", NULL, NULL);
    suite_page_alloc_tests(s);

    /* SCRUM-59: exhausts and refills the real PMM through the syscall path
     * (EXO_SYS_PAGE_ALLOC/_FREE) -- the physical-pool sibling of
     * "syscall_mem" and "page_alloc" above, kept next to them. */
    s = CU_add_suite("memory_isolation", NULL, NULL);
    suite_memory_isolation_tests(s);

    s = CU_add_suite("fb_binding", fb_binding_suite_init,
                     fb_binding_suite_cleanup);
    suite_fb_binding_tests(s);

    s = CU_add_suite("fb_shadow", fb_shadow_suite_init,
                     fb_shadow_suite_cleanup);
    suite_fb_shadow_tests(s);

    s = CU_add_suite("vmm", NULL, NULL);
    suite_vmm_tests(s);

    s = CU_add_suite("vmm_fb_wad", NULL, NULL);
    suite_vmm_fb_wad_tests(s);

    s = CU_add_suite("revoke", revoke_suite_init, revoke_suite_cleanup);
    suite_revoke_tests(s);

    s = CU_add_suite("page_map", NULL, page_map_suite_cleanup);
    suite_page_map_tests(s);

    s = CU_add_suite("fault", NULL, fault_suite_cleanup);
    suite_fault_tests(s);

    s = CU_add_suite("heap", NULL, NULL);
    suite_heap_tests(s);

    s = CU_add_suite("heap_stress", heap_stress_suite_init, NULL);
    suite_heap_stress_tests(s);

    s = CU_add_suite("tss", NULL, tss_suite_cleanup);
    suite_tss_tests(s);

    s = CU_add_suite("libos_launch", NULL, libos_launch_suite_cleanup);
    suite_libos_launch_tests(s);

    s = CU_add_suite("syscall_serial", NULL, NULL);
    suite_syscall_serial_tests(s);

    s = CU_add_suite("syscall_kbd", NULL, NULL);
    suite_syscall_kbd_tests(s);

    s = CU_add_suite("pit", NULL, NULL);
    suite_pit_tests(s);

    s = CU_add_suite("speaker", NULL, NULL);
    suite_speaker_tests(s);

    /* Right after speaker: these handlers sit directly on that driver, and
     * if it is failing its failures explain these. */
    s = CU_add_suite("syscall_sound", NULL, syscall_sound_suite_cleanup);
    suite_syscall_sound_tests(s);

    s = CU_add_suite("syscall_pit", NULL, NULL);
    suite_syscall_pit_tests(s);

    /* Right after syscall_pit: this suite sits directly on top of
     * exo_get_ticks, and if that one is failing its failures explain these. */
    s = CU_add_suite("doomgeneric_timer", NULL, NULL);
    suite_doomgeneric_timer_tests(s);

    s = CU_add_suite("libos_main", NULL, libos_main_suite_cleanup);
    suite_libos_main_tests(s);

    s = CU_add_suite("libos_c_probe", NULL, libos_c_probe_suite_cleanup);
    suite_libos_c_probe_tests(s);

    s = CU_add_suite("libc_shim_probe", libc_shim_probe_suite_init,
                     libc_shim_probe_suite_cleanup);
    suite_libc_shim_probe_tests(s);

    /* No cleanup: the suite's last test (test_slot_table_exhaustion) resets
     * src/libos_page_alloc.c's static state to empty as its final action,
     * which "libos_heap_stress" below relies on -- keep this suite
     * immediately before it. */
    s = CU_add_suite("libos_page_alloc", NULL, NULL);
    suite_libos_page_alloc_tests(s);

    s = CU_add_suite("libos_heap", NULL, NULL);
    suite_libos_heap_tests(s);

    s = CU_add_suite("libos_heap_stress", libos_heap_stress_suite_init, NULL);
    suite_libos_heap_stress_tests(s);

    s = CU_add_suite("port_io_fault", NULL, port_io_fault_suite_cleanup);
    suite_port_io_fault_tests(s);

    s = CU_add_suite("kernel_mem_fault", NULL, kernel_mem_fault_suite_cleanup);
    suite_kernel_mem_fault_tests(s);

    s = CU_add_suite("irq_entry", NULL, irq_entry_suite_cleanup);
    suite_irq_entry_tests(s);

    s = CU_add_suite("context", NULL, context_suite_cleanup);
    suite_context_tests(s);

    s = CU_add_suite("fb_console", fb_console_suite_init, fb_console_suite_cleanup);
    suite_fb_console_tests(s);

    s = CU_add_suite("context_switch", NULL, context_switch_suite_cleanup);
    suite_context_switch_tests(s);

    s = CU_add_suite("sse", sse_suite_init, sse_suite_cleanup);
    suite_sse_tests(s);

    s = CU_add_suite("syscall_yield", NULL, syscall_yield_suite_cleanup);
    suite_syscall_yield_tests(s);

    s = CU_add_suite("shell_libos", shell_libos_suite_init,
                     shell_libos_suite_cleanup);
    suite_shell_libos_tests(s);

    s = CU_add_suite("clock_libos", clock_libos_suite_init,
                     clock_libos_suite_cleanup);
    suite_clock_libos_tests(s);

    s = CU_add_suite("context_launch_rebind", NULL,
                     context_launch_rebind_suite_cleanup);
    suite_context_launch_rebind_tests(s);

    s = CU_add_suite("fixed_math", NULL, NULL);
    suite_fixed_math_tests(s);
    s = CU_add_suite("doom_panic", NULL, NULL);
    suite_doom_panic_tests(s);
    s = CU_add_suite("dg_init", NULL, NULL);
    suite_dg_init_tests(s);

    s = CU_add_suite("fpconv", NULL, NULL);
    suite_fpconv_tests(s);

    s = CU_add_suite("libc_gaps", NULL, NULL);
    suite_libc_gaps_tests(s);

    s = CU_add_suite("syscall_bench", syscall_bench_suite_init,
                     syscall_bench_suite_cleanup);
    suite_syscall_bench_tests(s);

    s = CU_add_suite("libos_snake", libos_snake_suite_init,
                     libos_snake_suite_cleanup);
    suite_libos_snake_tests(s);

    /* Pure function over one key -- no hardware, no state, so no suite
     * init/cleanup (SCRUM-40). */
    s = CU_add_suite("doom_keymap", NULL, NULL);
    suite_doom_keymap_tests(s);

    s = CU_add_suite("doom_sfx_tone", NULL, NULL);
    suite_doom_sfx_tone_tests(s);

    s = CU_add_suite("doom_sound", NULL, NULL);
    suite_doom_sound_tests(s);

    s = CU_add_suite("libos_doom", libos_doom_suite_init,
                     libos_doom_suite_cleanup);
    suite_libos_doom_tests(s);

    s = CU_add_suite("syscall_stat", NULL, syscall_stat_suite_cleanup);
    suite_syscall_stat_tests(s);

    s = CU_add_suite("ata", NULL, NULL);
    suite_ata_tests(s);

    s = CU_add_suite("syscall_disk", syscall_disk_suite_init,
                     syscall_disk_suite_cleanup);
    suite_syscall_disk_tests(s);

    s = CU_add_suite("disk_binding", disk_binding_suite_init,
                     disk_binding_suite_cleanup);
    suite_disk_binding_tests(s);

    /* Next to "ata" above: the other driver that talks to real emulated
     * hardware rather than to a model of it (SCRUM-209). */
    s = CU_add_suite("pci", NULL, NULL);
    suite_pci_tests(s);

    /* After "pci", which it depends on: hda_init() finds the controller
     * through pci_find_class() and maps its BAR (SCRUM-210). The cleanup
     * stops the stream -- these are the only tests that leave a DMA engine
     * and an unmasked IRQ line running while they measure. */
    s = CU_add_suite("hda", NULL, suite_hda_cleanup);
    suite_hda_tests(s);

    /* The other end of the same audio path: where HDA plays samples, this
     * decodes the ones Doom actually ships.  Reads the real freedoom2 module
     * for its reference data, hence the mount/unmount pair (SCRUM-211). */
    s = CU_add_suite("doom_dmx", suite_doom_dmx_init, suite_doom_dmx_cleanup);
    suite_doom_dmx_tests(s);

    /* Runs last: hammers exo_syscall_dispatch() with a million random
     * syscalls and checks the PMM is still sane afterward, so it should not
     * shadow which earlier suite actually broke a handler (SCRUM-115). */
    s = CU_add_suite("syscall_fuzz", NULL, NULL);
    suite_syscall_fuzz_tests(s);

    /* ADD NEW SUITES HERE: declare suite_*_tests above, then register it. */

    CU_run_all_tests();

    return CU_get_number_of_tests_failed() != 0 ? 1 : 0;
}
