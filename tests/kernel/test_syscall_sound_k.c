/*
 * test_syscall_sound_k.c — exo_sound_tone / exo_sound_stop (SCRUM-100).
 *
 * Acceptance: "sound goes through syscall; LibOS cannot directly access PIT
 * channel 2". The second half is the ring-3 test at the bottom: a real LibOS
 * address space runs tests/kernel/sound_port_probe.s, whose four direct
 * port accesses (0x43, 0x42, 0x61 read, 0x61 write) must each take #GP at
 * CPL 3, and whose exo_sound_tone through the real `syscall` instruction
 * must start the speaker. Everything above it drives the dispatcher from
 * ring 0 for the argument validation and the holder policy, and checks the
 * result against port 0x61 itself rather than trusting a return code.
 *
 * Every test leaves the speaker silent and the current context restored.
 */

#include "kunit.h"
#include "syscall.h"
#include "syscall_sound.h"
#include "exo_syscall.h"
#include "speaker.h"
#include "context.h"
#include "fault.h"
#include "libos_launch.h"
#include "libos_test_common.h"
#include "vmm.h"
#include "io.h"
#include "sleep.h"
#include "pit.h"

#include <stdint.h>

#define GATE_BITS 0x03

static int64_t do_tone(uint64_t freq, uint64_t dur_ms)
{
    return exo_syscall_dispatch(EXO_SYS_SOUND_TONE, freq, dur_ms, 0, 0, 0, 0);
}

static int64_t do_stop(void)
{
    return exo_syscall_dispatch(EXO_SYS_SOUND_STOP, 0, 0, 0, 0, 0, 0);
}

static int gate_open(void)
{
    return (inb(0x61) & GATE_BITS) == GATE_BITS;
}

static void test_handlers_are_bound(void)
{
    CU_ASSERT_PTR_NOT_NULL(exo_syscall_handler(EXO_SYS_SOUND_TONE));
    CU_ASSERT_PTR_NOT_NULL(exo_syscall_handler(EXO_SYS_SOUND_STOP));
}

static void test_tone_starts_speaker(void)
{
    CU_ASSERT_EQUAL(do_tone(440, 100), 0);
    CU_ASSERT_EQUAL(speaker_is_playing(), 1);
    CU_ASSERT_EQUAL(gate_open(), 1);

    CU_ASSERT_EQUAL(do_stop(), 0);
    CU_ASSERT_EQUAL(speaker_is_playing(), 0);
    CU_ASSERT_EQUAL(gate_open(), 0);
}

static void test_tone_rejects_bad_frequency(void)
{
    speaker_stop();

    CU_ASSERT_EQUAL(do_tone(0, 100), -EXO_EINVAL);
    CU_ASSERT_EQUAL(do_tone(SPEAKER_MIN_HZ - 1, 100), -EXO_EINVAL);
    CU_ASSERT_EQUAL(do_tone(SPEAKER_MAX_HZ + 1, 100), -EXO_EINVAL);
    /* Would be 440 after a 32-bit truncation; must be refused as given. */
    CU_ASSERT_EQUAL(do_tone((1ULL << 32) + 440, 100), -EXO_EINVAL);

    CU_ASSERT_EQUAL(speaker_is_playing(), 0);
    CU_ASSERT_EQUAL(gate_open(), 0);
}

static void test_tone_rejects_bad_duration(void)
{
    speaker_stop();

    /* 0 would be the driver's "until stopped" -- not offered to ring 3. */
    CU_ASSERT_EQUAL(do_tone(440, 0), -EXO_EINVAL);
    CU_ASSERT_EQUAL(do_tone(440, SOUND_TONE_MAX_MS + 1), -EXO_EINVAL);
    CU_ASSERT_EQUAL(do_tone(440, (1ULL << 32) + 100), -EXO_EINVAL);

    CU_ASSERT_EQUAL(speaker_is_playing(), 0);

    /* The boundary itself is accepted. */
    CU_ASSERT_EQUAL(do_tone(440, SOUND_TONE_MAX_MS), 0);
    CU_ASSERT_EQUAL(do_stop(), 0);
}

static void test_tone_is_non_blocking_and_ends_on_its_own(void)
{
    uint32_t before = kernel_get_ticks_ms();
    CU_ASSERT_EQUAL(do_tone(440, 20), 0);
    /* With IF clear no tick can land, so "returned at once" means the clock
     * has not moved at all across the call. */
    CU_ASSERT_EQUAL(kernel_get_ticks_ms(), before);
    CU_ASSERT_EQUAL(speaker_is_playing(), 1);

    __asm__ volatile ("sti");
    kernel_sleep_ms(40);
    __asm__ volatile ("cli");

    CU_ASSERT_EQUAL(speaker_is_playing(), 0);
    CU_ASSERT_EQUAL(gate_open(), 0);
}

static void test_stop_when_silent_is_ok(void)
{
    speaker_stop();
    CU_ASSERT_EQUAL(do_stop(), 0);
}

static void test_stop_refuses_another_contexts_tone(void)
{
    page_owner_t me = context_current();
    page_owner_t other = (page_owner_t)(PAGE_OWNER_LIBOS + 1);

    CU_ASSERT_EQUAL(do_tone(440, 1000), 0);

    context_set_current(other);
    CU_ASSERT_EQUAL(do_stop(), -EXO_EPERM);
    CU_ASSERT_EQUAL(speaker_is_playing(), 1);

    /* ...but it may replace it: last tone wins, and then the tone is its. */
    CU_ASSERT_EQUAL(do_tone(660, 1000), 0);
    CU_ASSERT_EQUAL(do_stop(), 0);
    CU_ASSERT_EQUAL(speaker_is_playing(), 0);

    context_set_current(me);
    speaker_stop();
}

static void test_release_silences_only_the_holder(void)
{
    page_owner_t me = context_current();
    page_owner_t other = (page_owner_t)(PAGE_OWNER_LIBOS + 1);

    CU_ASSERT_EQUAL(do_tone(440, 1000), 0);

    syscall_sound_release(other);
    CU_ASSERT_EQUAL(speaker_is_playing(), 1);

    syscall_sound_release(me);
    CU_ASSERT_EQUAL(speaker_is_playing(), 0);
    CU_ASSERT_EQUAL(gate_open(), 0);
}

/* ── Ring 3 ─────────────────────────────────────────────────────────────── */

extern void sound_port_probe(void);
extern void sound_port_probe_end(void);

#define PROBE_DIRECT_PORT_ACCESSES 4

static volatile int      gp_count;
static volatile uint64_t gp_cs_rpl_or;

static int skip_port_insn_hook(exception_frame_t *f, uint64_t cr2)
{
    (void)cr2;
    gp_count++;
    gp_cs_rpl_or |= ~f->cs & 3;   /* stays 0 only if every fault was CPL 3 */
    f->rip += 2;                   /* E4/E6 ib: 2 bytes, see the probe */
    return 1;
}

static void test_ring3_cannot_touch_speaker_ports_but_syscall_works(void)
{
    speaker_stop();

    size_t code_len = (uintptr_t)&sound_port_probe_end -
                      (uintptr_t)&sound_port_probe;

    libos_image_t img;
    CU_ASSERT_EQUAL(libos_build_image(TEST_OWNER_SOUND,
                                      (const void *)&sound_port_probe,
                                      code_len, 0, 0, 0, &img),
                    VMM_OK);

    gp_count = 0;
    gp_cs_rpl_or = 0;
    fault_set_hook(skip_port_insn_hook);
    exo_syscall_register(LIBOS_RETURN_SYSCALL_NUM, libos_return);

    CU_ASSERT_EQUAL(vmm_switch_address_space(img.pml4_phys), VMM_OK);
    uint64_t result = libos_enter(img.entry_vaddr, img.stack_top_vaddr);
    CU_ASSERT_EQUAL(vmm_switch_address_space(vmm_kernel_pml4()), VMM_OK);

    exo_syscall_register(LIBOS_RETURN_SYSCALL_NUM, 0);
    fault_set_hook(0);

    /* Every direct access trapped, and each one from ring 3. */
    CU_ASSERT_EQUAL(gp_count, PROBE_DIRECT_PORT_ACCESSES);
    CU_ASSERT_EQUAL(gp_cs_rpl_or, 0);

    /* The syscall is what worked: its genuine return value came back, and
     * the speaker is sounding because of it, not because of the outb. */
    CU_ASSERT_EQUAL((int64_t)result, 0);
    CU_ASSERT_EQUAL(speaker_is_playing(), 1);
    CU_ASSERT_EQUAL(gate_open(), 1);

    speaker_stop();
    libos_destroy_image(TEST_OWNER_SOUND, &img);
}

int syscall_sound_suite_cleanup(void)
{
    speaker_stop();
    libos_test_teardown_owner(TEST_OWNER_SOUND);
    return 0;
}

void suite_syscall_sound_tests(CU_pSuite s)
{
    CU_add_test(s, "handlers are bound", test_handlers_are_bound);
    CU_add_test(s, "tone starts speaker", test_tone_starts_speaker);
    CU_add_test(s, "tone rejects bad frequency",
               test_tone_rejects_bad_frequency);
    CU_add_test(s, "tone rejects bad duration",
               test_tone_rejects_bad_duration);
    CU_add_test(s, "tone is non-blocking and ends on its own",
               test_tone_is_non_blocking_and_ends_on_its_own);
    CU_add_test(s, "stop when silent is ok", test_stop_when_silent_is_ok);
    CU_add_test(s, "stop refuses another context's tone",
               test_stop_refuses_another_contexts_tone);
    CU_add_test(s, "release silences only the holder",
               test_release_silences_only_the_holder);
    CU_add_test(s, "ring 3 cannot touch speaker ports, syscall works",
               test_ring3_cannot_touch_speaker_ports_but_syscall_works);
}
