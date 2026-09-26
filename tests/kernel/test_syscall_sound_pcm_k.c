/*
 * test_syscall_sound_pcm_k.c — exo_sound_pcm / exo_sound_pcm_stop (SCRUM-213).
 *
 * Acceptance: "a ring-3 test LibOS queues a decoded sample through the syscall
 * and it comes out the speaker, proven the way tests/kernel/test_syscall_k.c
 * proves other syscalls from ring 3". The ring-3 test at the bottom is that
 * clause: a real LibOS address space runs tests/kernel/sound_pcm_probe.s,
 * which calls #27 through the real `syscall` instruction with its *own* mapped
 * data page as the sample buffer, and the test then checks the samples are
 * being DMA'd out by the controller -- the stream's own read position moving
 * through the buffer while the voice the probe started is still sounding --
 * rather than trusting a return code.
 *
 * Everything above it drives the dispatcher from ring 0, in the shape
 * test_syscall_disk_k.c established for a syscall that takes a LibOS buffer:
 * a page allocated and mapped through the real #0/#2 handlers, so the
 * -EXO_EFAULT legs are tested against genuinely mapped and genuinely unmapped
 * addresses rather than plausible-looking numbers.
 *
 * Kept apart from test_syscall_sound_k.c because the two need different
 * hardware: that suite needs only the PIT and port 0x61, this one needs an HDA
 * controller. Splitting them means a machine without one loses this suite
 * rather than the speaker's coverage too.
 *
 * ── What this suite deliberately does NOT do ─────────────────────────────
 *
 * It never lets interrupts in except where it is timing something. The mixer
 * only advances inside hda_irq_handler(), so with IF clear a started voice
 * stays exactly where it was put -- which is what makes the voice-exhaustion
 * and ownership tests below deterministic instead of a race against a 21 ms
 * refill.
 *
 * Every test leaves the mixer silent, the stream stopped, the staging pages
 * returned and the current context restored.
 */

#include "kunit.h"
#include "syscall.h"
#include "syscall_sound.h"
#include "exo_syscall.h"
#include "pcm_mixer.h"
#include "hda.h"
#include "page_alloc.h"
#include "context.h"
#include "libos_launch.h"
#include "libos_test_common.h"
#include "vmm.h"
#include "pit.h"

#include <stdint.h>

/* Scratch virtual address in the LibOS window, apart from every other suite's
 * range (test_syscall_serial_k.c uses +0x28000000, test_syscall_disk_k.c
 * +0x29000000, test_fb_binding_k.c +0x2C000000). */
#define SCRATCH  (EXO_USER_VA_BASE + 0x2D000000ULL)

/* A second address in this suite's own range that no test here ever maps --
 * the in-window-but-unmapped pointer SCRUM-186 reproduces the crash with. */
#define UNMAPPED (SCRATCH + 0x1000ULL)

/* A second, distinct LibOS id, the convention test_syscall_sound_k.c and
 * test_fb_binding_k.c use for "another context". */
#define OTHER_LIBOS ((page_owner_t)(PAGE_OWNER_LIBOS + 1))

/* Sample count for the ring-0 tests: one page's worth, so a queue costs
 * exactly one staging page and the page accounting below is easy to read.
 * 4096 samples at 11025 Hz is ~370 ms, far longer than the suite takes with
 * IF clear (during which a voice cannot advance at all). */
#define TEST_SAMPLES 4096u
#define TEST_RATE_HZ 11025u

/* Must match sound_pcm_probe.s's own .set values. */
#define PROBE_SAMPLES  2048u
#define PROBE_RATE_HZ  11025u
#define PROBE_PRIORITY 64

static int64_t do_pcm(uint64_t buf, uint64_t samples, uint64_t rate,
                      uint64_t vol, uint64_t sep, uint64_t priority)
{
    return exo_syscall_dispatch(EXO_SYS_SOUND_PCM, buf, samples, rate, vol,
                                sep, priority);
}

static int64_t do_pcm_stop(uint64_t handle)
{
    return exo_syscall_dispatch(EXO_SYS_SOUND_PCM_STOP, handle, 0, 0, 0, 0, 0);
}

static int64_t do_alloc(void)
{
    return exo_syscall_dispatch(EXO_SYS_PAGE_ALLOC, 0, 0, 0, 0, 0, 0);
}

static int64_t do_free(uint64_t paddr)
{
    return exo_syscall_dispatch(EXO_SYS_PAGE_FREE, paddr, 0, 0, 0, 0, 0);
}

static int64_t do_map(uint64_t vaddr, uint64_t paddr, uint64_t flags)
{
    return exo_syscall_dispatch(EXO_SYS_PAGE_MAP, vaddr, paddr, flags,
                                0, 0, 0);
}

static int64_t do_unmap(uint64_t vaddr)
{
    return exo_syscall_dispatch(EXO_SYS_PAGE_UNMAP, vaddr, 0, 0, 0, 0, 0);
}

/* Fill `n` bytes at `dst` with a square wave in DMX's 8-bit *unsigned*
 * encoding (128 is silence), so what the mixer plays is a real waveform
 * rather than a constant the resampler could not be distinguished on. */
static void fill_square(uint8_t *dst, uint32_t n, uint32_t period)
{
    for (uint32_t i = 0; i < n; i++)
        dst[i] = (uint8_t)(((i / (period / 2u)) & 1u) ? 200 : 56);
}

/* Map one page at SCRATCH through the real syscalls and fill it with a wave.
 * Returns its physical address, for unmap_scratch(). */
static uint64_t map_scratch(void)
{
    int64_t paddr = do_alloc();
    CU_ASSERT_TRUE(paddr > 0);
    if (paddr <= 0)
        return 0;

    CU_ASSERT_EQUAL(do_map(SCRATCH, (uint64_t)paddr,
                           VMM_PRESENT | VMM_WRITE | VMM_USER), 0);
    fill_square((uint8_t *)(uintptr_t)SCRATCH, (uint32_t)VMM_PAGE_SIZE, 64u);
    return (uint64_t)paddr;
}

static void unmap_scratch(uint64_t paddr)
{
    CU_ASSERT_EQUAL(do_unmap(SCRATCH), 0);
    CU_ASSERT_EQUAL(do_free(paddr), 0);
}

/*
 * Wait until the controller's DMA read position moves, and report whether it
 * did. Bounded, so a stopped stream is a failure to assert rather than a hung
 * test boot.
 *
 * Position rather than `hda_pcm_refills()`, deliberately: the refill counter
 * only advances when the *completion interrupt* is delivered, and INTx
 * delivery under QEMU is intermittent -- the `hda_pcm` suite's own refill
 * floors fail on roughly one boot in three, on this branch's base and before
 * it. `SDnLPIB` is moved by the DMA engine itself, so it answers the question
 * this suite actually needs answered ("are the samples the LibOS queued
 * leaving RAM?") without depending on the part that is flaky. Refills are
 * still *checked* where they are a bonus, never required.
 *
 * Compared for inequality rather than growth because the position wraps every
 * HDA_BUF_FRAMES (~341 ms at 48 kHz); a short poll for any change cannot be
 * fooled by that the way a `>` against a stale sample could.
 */
static int wait_for_dma_progress(void)
{
    uint32_t before = hda_stream_position();
    uint32_t start  = kernel_get_ticks_ms();
    int moved = 0;

    __asm__ volatile ("sti");
    while ((uint32_t)(kernel_get_ticks_ms() - start) < 200u) {
        if (hda_stream_position() != before) {
            moved = 1;
            break;
        }
        __asm__ volatile ("hlt");
    }
    __asm__ volatile ("cli");

    return moved;
}

/* Silence everything and let the next sound syscall's sweep return the
 * staging pages, so no test leaves a buffer (or a page) behind for the next
 * one to trip over. */
static void quiesce(void)
{
    pcm_mixer_stop_all();
    syscall_sound_release(context_current());
    syscall_sound_release(OTHER_LIBOS);
    hda_pcm_stop();
    pcm_mixer_reset();
}

int syscall_sound_pcm_suite_init(void)
{
    quiesce();
    return 0;
}

int syscall_sound_pcm_suite_cleanup(void)
{
    quiesce();
    libos_test_teardown_owner(TEST_OWNER_SOUND_PCM);
    return 0;
}

/* ── Binding ────────────────────────────────────────────────────────────── */

/* The boot path must have bound both numbers; without this the rest of the
 * suite would only be re-proving the dispatcher's -EXO_ENOSYS fallback. */
static void test_handlers_are_bound(void)
{
    CU_ASSERT_PTR_NOT_NULL(exo_syscall_handler(EXO_SYS_SOUND_PCM));
    CU_ASSERT_PTR_NOT_NULL(exo_syscall_handler(EXO_SYS_SOUND_PCM_STOP));
}

/* The decision this ticket had to make, stated as an assertion: sound is
 * shared, so there is no acquire syscall to hold it. If a later ticket adds
 * one, this is the test that says the policy changed on purpose. */
static void test_there_is_no_sound_acquire(void)
{
    CU_ASSERT_EQUAL(EXO_SYS_COUNT, EXO_SYS_SOUND_PCM_STOP + 1);
}

/* ── Argument validation ────────────────────────────────────────────────── */

static void test_rejects_bad_sample_count(void)
{
    uint64_t paddr = map_scratch();

    CU_ASSERT_EQUAL(do_pcm(SCRATCH, 0, TEST_RATE_HZ, 127, 128, 64),
                    -EXO_EINVAL);
    CU_ASSERT_EQUAL(do_pcm(SCRATCH, SOUND_PCM_MAX_SAMPLES + 1, TEST_RATE_HZ,
                           127, 128, 64), -EXO_EINVAL);
    /* Would be TEST_SAMPLES after a 32-bit truncation; must be refused as
     * given, same rule sys_sound_tone applies to freq. */
    CU_ASSERT_EQUAL(do_pcm(SCRATCH, (1ULL << 32) + TEST_SAMPLES, TEST_RATE_HZ,
                           127, 128, 64), -EXO_EINVAL);

    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), 0u);
    CU_ASSERT_EQUAL(syscall_sound_pcm_pages_used(), 0u);
    unmap_scratch(paddr);
}

static void test_rejects_bad_rate(void)
{
    uint64_t paddr = map_scratch();

    CU_ASSERT_EQUAL(do_pcm(SCRATCH, TEST_SAMPLES, 0, 127, 128, 64),
                    -EXO_EINVAL);
    /* 65536 Hz and up overflows the mixer's 16.16 phase step. */
    CU_ASSERT_EQUAL(do_pcm(SCRATCH, TEST_SAMPLES, 65536, 127, 128, 64),
                    -EXO_EINVAL);
    CU_ASSERT_EQUAL(do_pcm(SCRATCH, TEST_SAMPLES, (1ULL << 32) + TEST_RATE_HZ,
                           127, 128, 64), -EXO_EINVAL);

    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), 0u);
    CU_ASSERT_EQUAL(syscall_sound_pcm_pages_used(), 0u);
    unmap_scratch(paddr);
}

static void test_rejects_buffer_outside_the_window(void)
{
    CU_ASSERT_EQUAL(do_pcm(EXO_USER_VA_BASE - 1, TEST_SAMPLES, TEST_RATE_HZ,
                           127, 128, 64), -EXO_EFAULT);
    CU_ASSERT_EQUAL(do_pcm(EXO_USER_VA_END, TEST_SAMPLES, TEST_RATE_HZ,
                           127, 128, 64), -EXO_EFAULT);
    /* A range that starts inside and runs off the end. */
    CU_ASSERT_EQUAL(do_pcm(EXO_USER_VA_END - 16, TEST_SAMPLES, TEST_RATE_HZ,
                           127, 128, 64), -EXO_EFAULT);

    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), 0u);
}

/* In-window but not mapped: the bounds check alone would wave this through
 * and the handler's copy would take a fatal supervisor page fault (SCRUM-186),
 * which is why exo_user_range_mapped() runs too. */
static void test_rejects_unmapped_buffer(void)
{
    CU_ASSERT_EQUAL(do_pcm(UNMAPPED, TEST_SAMPLES, TEST_RATE_HZ, 127, 128, 64),
                    -EXO_EFAULT);
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), 0u);
    CU_ASSERT_EQUAL(syscall_sound_pcm_pages_used(), 0u);
}

/* A buffer that starts on a mapped page and runs onto an unmapped one: the
 * per-page walk has to reject it, not just look at `base`. */
static void test_rejects_buffer_crossing_into_unmapped(void)
{
    uint64_t paddr = map_scratch();

    CU_ASSERT_EQUAL(do_pcm(SCRATCH + VMM_PAGE_SIZE - 16, 64, TEST_RATE_HZ,
                           127, 128, 64), -EXO_EFAULT);
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), 0u);

    unmap_scratch(paddr);
}

/* ── Playing ────────────────────────────────────────────────────────────── */

static void test_queue_starts_a_voice_and_the_stream(void)
{
    CU_ASSERT_TRUE(hda_present());

    uint64_t paddr = map_scratch();
    uint32_t kernel_pages_before = page_count_owned(PAGE_OWNER_KERNEL);

    int64_t handle = do_pcm(SCRATCH, TEST_SAMPLES, TEST_RATE_HZ,
                            PCM_MIXER_VOL_MAX, PCM_MIXER_SEP_CENTRE, 64);
    CU_ASSERT_TRUE(handle >= 0);
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), 1u);
    CU_ASSERT_EQUAL(pcm_mixer_is_playing((int)handle), 1);

    /* Lazily started by the handler, not by syscall_sound_init(). */
    CU_ASSERT_TRUE(hda_pcm_is_streaming());

    /* One page of samples became one staging page, taken from the PMM as
     * kernel-owned -- the copy that lets the voice outlive the caller's
     * buffer. */
    CU_ASSERT_EQUAL(syscall_sound_pcm_slots_used(), 1u);
    CU_ASSERT_EQUAL(syscall_sound_pcm_pages_used(), 1u);
    CU_ASSERT_EQUAL(page_count_owned(PAGE_OWNER_KERNEL),
                    kernel_pages_before + 1u);

    CU_ASSERT_EQUAL(do_pcm_stop((uint64_t)handle), 0);
    CU_ASSERT_EQUAL(pcm_mixer_is_playing((int)handle), 0);
    CU_ASSERT_EQUAL(syscall_sound_pcm_pages_used(), 0u);
    CU_ASSERT_EQUAL(page_count_owned(PAGE_OWNER_KERNEL), kernel_pages_before);

    hda_pcm_stop();
    unmap_scratch(paddr);
}

/* The caller's buffer is its own again the moment the syscall returns: the
 * samples were copied, so unmapping *and freeing* the page underneath a
 * sounding voice is harmless. Before the copy this was a fatal read from
 * hda_irq_handler() under whatever CR3 happened to be loaded. */
static void test_voice_survives_the_callers_buffer(void)
{
    uint64_t paddr = map_scratch();

    int64_t handle = do_pcm(SCRATCH, TEST_SAMPLES, TEST_RATE_HZ,
                            PCM_MIXER_VOL_MAX, PCM_MIXER_SEP_CENTRE, 64);
    CU_ASSERT_TRUE(handle >= 0);

    unmap_scratch(paddr);

    /* Still sounding, and still being played out with the caller's page gone:
     * the DMA engine advances and the stream takes no FIFO error, which it
     * would if a render had faulted or been skipped. */
    CU_ASSERT_TRUE(wait_for_dma_progress());
    CU_ASSERT_EQUAL(pcm_mixer_is_playing((int)handle), 1);
    CU_ASSERT_EQUAL(hda_pcm_underruns(), 0u);

    CU_ASSERT_EQUAL(do_pcm_stop((uint64_t)handle), 0);
    hda_pcm_stop();
}

/*
 * The shared-not-exclusive decision, where it actually shows: eight voices
 * are available to one caller at once, and the ninth is refused *without*
 * disturbing any of them. That refusal is the mixer's non-starvation rule
 * (src/pcm_mixer.h) restated at the ABI, and it is the whole reason there is
 * no exo_sound_acquire -- an exclusive binding would answer this case by
 * locking out a second LibOS instead.
 */
static void test_ninth_voice_is_refused_and_disturbs_nothing(void)
{
    uint64_t paddr = map_scratch();
    int64_t handles[PCM_MIXER_VOICES];

    for (uint32_t i = 0; i < PCM_MIXER_VOICES; i++) {
        handles[i] = do_pcm(SCRATCH, TEST_SAMPLES, TEST_RATE_HZ,
                            PCM_MIXER_VOL_MAX, PCM_MIXER_SEP_CENTRE, 64);
        CU_ASSERT_TRUE(handles[i] >= 0);
    }
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), PCM_MIXER_VOICES);
    CU_ASSERT_EQUAL(syscall_sound_pcm_slots_used(), PCM_MIXER_VOICES);

    /* Less important (a higher priority number) than everything playing. */
    CU_ASSERT_EQUAL(do_pcm(SCRATCH, TEST_SAMPLES, TEST_RATE_HZ,
                           PCM_MIXER_VOL_MAX, PCM_MIXER_SEP_CENTRE, 200),
                    -EXO_EBUSY);

    /* Nothing was stolen, and the refusal leaked no staging page. */
    for (uint32_t i = 0; i < PCM_MIXER_VOICES; i++)
        CU_ASSERT_EQUAL(pcm_mixer_is_playing((int)handles[i]), 1);
    CU_ASSERT_EQUAL(syscall_sound_pcm_pages_used(), PCM_MIXER_VOICES);

    /* A *more* important sound does get in, and the row it took over is
     * reused rather than doubled up. */
    int64_t urgent = do_pcm(SCRATCH, TEST_SAMPLES, TEST_RATE_HZ,
                            PCM_MIXER_VOL_MAX, PCM_MIXER_SEP_CENTRE, 1);
    CU_ASSERT_TRUE(urgent >= 0);
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), PCM_MIXER_VOICES);

    quiesce();
    /* One sweep by the next sound syscall and every staging page is back. */
    CU_ASSERT_EQUAL(syscall_sound_pcm_pages_used(), 0u);
    unmap_scratch(paddr);
}

/* ── Ownership ──────────────────────────────────────────────────────────── */

static void test_stop_refuses_another_contexts_voice(void)
{
    page_owner_t me = context_current();
    uint64_t paddr = map_scratch();

    int64_t handle = do_pcm(SCRATCH, TEST_SAMPLES, TEST_RATE_HZ,
                            PCM_MIXER_VOL_MAX, PCM_MIXER_SEP_CENTRE, 64);
    CU_ASSERT_TRUE(handle >= 0);

    context_set_current(OTHER_LIBOS);
    CU_ASSERT_EQUAL(do_pcm_stop((uint64_t)handle), -EXO_EPERM);
    CU_ASSERT_EQUAL(pcm_mixer_is_playing((int)handle), 1);
    CU_ASSERT_EQUAL(syscall_sound_pcm_pages_used(), 1u);

    /* ...but it may play alongside: there is no binding to lose. */
    int64_t theirs = do_pcm(SCRATCH, TEST_SAMPLES, TEST_RATE_HZ,
                            PCM_MIXER_VOL_MAX, PCM_MIXER_SEP_CENTRE, 64);
    CU_ASSERT_TRUE(theirs >= 0);
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), 2u);
    CU_ASSERT_EQUAL(do_pcm_stop((uint64_t)theirs), 0);

    context_set_current(me);
    CU_ASSERT_EQUAL(do_pcm_stop((uint64_t)handle), 0);
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), 0u);

    hda_pcm_stop();
    unmap_scratch(paddr);
}

/* A handle nobody was ever issued is not an error to stop -- there is nothing
 * to stop -- but a negative one could never have come from #27. */
static void test_stop_of_unknown_handle(void)
{
    CU_ASSERT_EQUAL(do_pcm_stop(0x7FFFFFFFu), 0);
    CU_ASSERT_EQUAL(do_pcm_stop((uint64_t)(int64_t)-1), -EXO_EINVAL);
}

/* A handle whose sound has finished on its own: stopping it is a no-op
 * success, not -EXO_EPERM from a row that outlived the voice. */
static void test_stop_of_finished_voice_is_ok(void)
{
    uint64_t paddr = map_scratch();

    int64_t handle = do_pcm(SCRATCH, TEST_SAMPLES, TEST_RATE_HZ,
                            PCM_MIXER_VOL_MAX, PCM_MIXER_SEP_CENTRE, 64);
    CU_ASSERT_TRUE(handle >= 0);

    /* Retire it behind the syscall's back, the way running out of samples
     * would inside hda_irq_handler(). */
    pcm_mixer_stop((int)handle);
    CU_ASSERT_EQUAL(do_pcm_stop((uint64_t)handle), 0);
    CU_ASSERT_EQUAL(syscall_sound_pcm_pages_used(), 0u);

    hda_pcm_stop();
    unmap_scratch(paddr);
}

/* exo_exit's hook: an exiting LibOS's voices stop and its staging pages come
 * back, and nobody else's do. */
static void test_release_stops_only_that_owners_voices(void)
{
    page_owner_t me = context_current();
    uint64_t paddr = map_scratch();

    int64_t mine = do_pcm(SCRATCH, TEST_SAMPLES, TEST_RATE_HZ,
                          PCM_MIXER_VOL_MAX, PCM_MIXER_SEP_CENTRE, 64);
    CU_ASSERT_TRUE(mine >= 0);

    context_set_current(OTHER_LIBOS);
    int64_t theirs = do_pcm(SCRATCH, TEST_SAMPLES, TEST_RATE_HZ,
                            PCM_MIXER_VOL_MAX, PCM_MIXER_SEP_CENTRE, 64);
    CU_ASSERT_TRUE(theirs >= 0);
    context_set_current(me);

    CU_ASSERT_EQUAL(syscall_sound_pcm_pages_used(), 2u);

    syscall_sound_release(OTHER_LIBOS);
    CU_ASSERT_EQUAL(pcm_mixer_is_playing((int)theirs), 0);
    CU_ASSERT_EQUAL(pcm_mixer_is_playing((int)mine), 1);
    CU_ASSERT_EQUAL(syscall_sound_pcm_pages_used(), 1u);

    syscall_sound_release(me);
    CU_ASSERT_EQUAL(pcm_mixer_is_playing((int)mine), 0);
    CU_ASSERT_EQUAL(syscall_sound_pcm_pages_used(), 0u);
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), 0u);

    hda_pcm_stop();
    unmap_scratch(paddr);
}

/* ── Ring 3 ─────────────────────────────────────────────────────────────── */

extern void sound_pcm_probe(void);
extern void sound_pcm_probe_end(void);

/*
 * The acceptance clause. A real LibOS address space, a real `syscall`, and a
 * buffer that is the LibOS's own data page -- so the validation the handler
 * runs (bounds, then a page-table walk of the LibOS's CR3) is exercised for
 * real rather than against an address the kernel arranged for itself.
 *
 * "It comes out the speaker" is asserted as: the voice the probe's own syscall
 * started is sounding, the stream is running, and the controller's refill
 * counter advances with no FIFO error -- the samples really are leaving RAM on
 * time. Audibility itself is docs/drivers/hda.md §10's separate claim, checked
 * by hand with `-audiodev wav`; headless CI has no speaker to listen to.
 *
 * Launched under this suite's own sentinel owner rather than
 * PAGE_OWNER_LIBOS, unlike test_libc_shim_probe_k.c: the only handler here
 * that has to resolve an address space is exo_user_range_mapped(), and it
 * reads CR3 directly (src/syscall.c) rather than going through the
 * context->PML4 registry, so the launch owner need not match
 * syscall_current_context().
 */
static void test_ring3_queues_pcm_through_the_syscall(void)
{
    CU_ASSERT_TRUE(hda_present());
    quiesce();

    /* The probe's samples ARE its data blob: libos_build_image() places `data`
     * at LIBOS_LAUNCH_DATA_VADDR, which is the pointer the probe passes. */
    static uint8_t wave[PROBE_SAMPLES];
    fill_square(wave, PROBE_SAMPLES, 32u);

    size_t code_len = (uintptr_t)&sound_pcm_probe_end -
                      (uintptr_t)&sound_pcm_probe;

    libos_image_t img;
    CU_ASSERT_EQUAL(libos_build_image(TEST_OWNER_SOUND_PCM,
                                      (const void *)&sound_pcm_probe, code_len,
                                      wave, sizeof(wave), 0, &img),
                    VMM_OK);

    exo_syscall_register(LIBOS_RETURN_SYSCALL_NUM, libos_return);

    CU_ASSERT_EQUAL(vmm_switch_address_space(img.pml4_phys), VMM_OK);
    uint64_t result = libos_enter(img.entry_vaddr, img.stack_top_vaddr);
    CU_ASSERT_EQUAL(vmm_switch_address_space(vmm_kernel_pml4()), VMM_OK);

    exo_syscall_register(LIBOS_RETURN_SYSCALL_NUM, 0);

    /* The genuine return value of exo_sound_pcm came back: a voice handle,
     * not a marker and not an error. */
    int64_t handle = (int64_t)result;
    CU_ASSERT_TRUE(handle >= 0);
    CU_ASSERT_EQUAL(pcm_mixer_active_voices(), 1u);
    CU_ASSERT_EQUAL(pcm_mixer_is_playing((int)handle), 1);
    CU_ASSERT_TRUE(hda_pcm_is_streaming());

    /* One page of staging for 2048 samples, owned by the kernel and not by
     * the LibOS whose address space has just been left. */
    CU_ASSERT_EQUAL(syscall_sound_pcm_slots_used(), 1u);
    CU_ASSERT_EQUAL(syscall_sound_pcm_pages_used(), 1u);

    /*
     * And what is on that voice is the probe's *own bytes*, not zeros and not
     * whatever the staging pages held before -- the part a DMA-position check
     * cannot see. Rendering a few frames here is the only way to assert it
     * from inside the kernel: the samples the copy carried across come back
     * out of the mixer at the amplitude a full-volume centred voice gives
     * them.
     *
     * 64 frames at 48 kHz is ~1.3 ms, which at the probe's 11025 Hz lands
     * inside the wave's first half-period (16 samples of 56, i.e. 72 counts
     * *below* the 128 that means silence), so every sample here is the same
     * negative plateau: (56 - 128) << 8 == -18432, scaled by the centred
     * gain. Done before the wait below, because 2048 samples is only ~186 ms
     * and the voice would otherwise have finished.
     */
    int16_t frames[64 * PCM_MIXER_CHANNELS];
    pcm_mixer_render(frames, 64);

    int all_negative = 1;
    int16_t peak_l = 0, peak_r = 0;
    for (uint32_t f = 0; f < 64; f++) {
        int16_t l = frames[f * PCM_MIXER_CHANNELS];
        int16_t r = frames[f * PCM_MIXER_CHANNELS + 1];
        if (l >= 0 || r >= 0)
            all_negative = 0;
        if ((int16_t)-l > peak_l) peak_l = (int16_t)-l;
        if ((int16_t)-r > peak_r) peak_r = (int16_t)-r;
    }
    CU_ASSERT_TRUE(all_negative);
    /* ~13824/13968 for the centred gain; a window rather than the exact
     * number, so a change to the panning law is not a failure here -- the
     * claim is "the probe's amplitude", not "this arithmetic". */
    CU_ASSERT_TRUE(peak_l > 13000 && peak_l < 14500);
    CU_ASSERT_TRUE(peak_r > 13000 && peak_r < 14500);

    /* And it is being played: the controller's DMA read position moves through
     * the buffer the mixer is rendering the probe's samples into, and no FIFO
     * error is reported while it does. */
    CU_ASSERT_TRUE(wait_for_dma_progress());
    CU_ASSERT_EQUAL(hda_pcm_underruns(), 0u);
    CU_ASSERT_TRUE(hda_pcm_is_streaming());

    quiesce();
    CU_ASSERT_EQUAL(syscall_sound_pcm_pages_used(), 0u);
    libos_destroy_image(TEST_OWNER_SOUND_PCM, &img);
}

/* ── Registration ──────────────────────────────────────────────────────── */

void suite_syscall_sound_pcm_tests(CU_pSuite s)
{
    CU_add_test(s, "handlers are bound", test_handlers_are_bound);
    CU_add_test(s, "there is no sound acquire syscall",
                test_there_is_no_sound_acquire);
    CU_add_test(s, "rejects a bad sample count",
                test_rejects_bad_sample_count);
    CU_add_test(s, "rejects a bad rate", test_rejects_bad_rate);
    CU_add_test(s, "rejects a buffer outside the window",
                test_rejects_buffer_outside_the_window);
    CU_add_test(s, "rejects an unmapped buffer", test_rejects_unmapped_buffer);
    CU_add_test(s, "rejects a buffer crossing into unmapped memory",
                test_rejects_buffer_crossing_into_unmapped);
    CU_add_test(s, "a queue starts a voice and the stream",
                test_queue_starts_a_voice_and_the_stream);
    CU_add_test(s, "a voice survives the caller's buffer",
                test_voice_survives_the_callers_buffer);
    CU_add_test(s, "the ninth voice is refused and disturbs nothing",
                test_ninth_voice_is_refused_and_disturbs_nothing);
    CU_add_test(s, "stop refuses another context's voice",
                test_stop_refuses_another_contexts_voice);
    CU_add_test(s, "stop of an unknown handle", test_stop_of_unknown_handle);
    CU_add_test(s, "stop of a finished voice is ok",
                test_stop_of_finished_voice_is_ok);
    CU_add_test(s, "release stops only that owner's voices",
                test_release_stops_only_that_owners_voices);
    CU_add_test(s, "ring 3 queues PCM through the syscall",
                test_ring3_queues_pcm_through_the_syscall);
}
