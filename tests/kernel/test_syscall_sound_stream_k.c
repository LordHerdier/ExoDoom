/*
 * test_syscall_sound_stream_k.c — exo_sound_stream_write / _stop / _volume
 * (#32-#34, SCRUM-219).
 *
 * The ring-3 test at the bottom is the acceptance clause: a real LibOS runs
 * tests/kernel/sound_stream_probe.s, which calls #32 through the real
 * `syscall` instruction with its own data page as the frames.  Everything
 * above drives the dispatcher from ring 0, in the shape of
 * test_syscall_sound_pcm_k.c.
 *
 * Interrupts stay off except in the ring-3 test's DMA wait, so the mixer
 * never drains the ring behind an assertion: with IF clear, the fill level
 * is exactly what the tests put there.
 *
 * Every test leaves the stream released, the mixer reset and the current
 * context restored.
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

#include <stdint.h>

/* Apart from every other suite's scratch range (sound_pcm uses +0x2D000000). */
#define SCRATCH  (EXO_USER_VA_BASE + 0x2E000000ULL)
#define UNMAPPED (SCRATCH + 0x4000ULL)

/* Scratch is SCRATCH_PAGES contiguous-virtual pages: 4 pages = 1024 frames,
 * enough for every ring-0 test that is not about filling the ring. */
#define SCRATCH_PAGES   4u
#define SCRATCH_FRAMES  (SCRATCH_PAGES * VMM_PAGE_SIZE / 4u)

/* Must match sound_stream_probe.s's PROBE_FRAMES. */
#define PROBE_FRAMES    1024u

static page_owner_t g_other = PAGE_OWNER_FREE;
static uint64_t     g_other_pml4;
#define OTHER_LIBOS g_other

static int64_t do_write(uint64_t buf, uint64_t frames)
{
    return exo_syscall_dispatch(EXO_SYS_SOUND_STREAM_WRITE, buf, frames,
                                0, 0, 0, 0);
}

static int64_t do_stop(void)
{
    return exo_syscall_dispatch(EXO_SYS_SOUND_STREAM_STOP, 0, 0, 0, 0, 0, 0);
}

static int64_t do_volume(uint64_t vol)
{
    return exo_syscall_dispatch(EXO_SYS_SOUND_STREAM_VOLUME, vol, 0, 0, 0, 0, 0);
}

static uint32_t accepted(int64_t r) { return (uint32_t)((uint64_t)r & 0xFFFFFFFFu); }
static uint32_t space(int64_t r)    { return (uint32_t)((uint64_t)r >> 32); }

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
    return exo_syscall_dispatch(EXO_SYS_PAGE_MAP, vaddr, paddr, flags, 0, 0, 0);
}
static int64_t do_unmap(uint64_t vaddr)
{
    return exo_syscall_dispatch(EXO_SYS_PAGE_UNMAP, vaddr, 0, 0, 0, 0, 0);
}

static uint64_t g_phys[SCRATCH_PAGES];

/* Map SCRATCH_PAGES pages at SCRATCH and fill them with a ramp, so a copy
 * that lands in the ring can be told from zeros. */
static void map_scratch(void)
{
    for (uint32_t i = 0; i < SCRATCH_PAGES; i++) {
        int64_t p = do_alloc();
        CU_ASSERT_TRUE(p > 0);
        g_phys[i] = p > 0 ? (uint64_t)p : 0;
        if (p > 0)
            CU_ASSERT_EQUAL(do_map(SCRATCH + i * VMM_PAGE_SIZE, (uint64_t)p,
                                   VMM_PRESENT | VMM_WRITE | VMM_USER), 0);
    }
    int16_t *s = (int16_t *)(uintptr_t)SCRATCH;
    for (uint32_t i = 0; i < SCRATCH_FRAMES * PCM_MIXER_CHANNELS; i++)
        s[i] = (int16_t)(1000 + (i & 0xFF));
}

static void unmap_scratch(void)
{
    for (uint32_t i = 0; i < SCRATCH_PAGES; i++) {
        if (g_phys[i] == 0)
            continue;
        CU_ASSERT_EQUAL(do_unmap(SCRATCH + i * VMM_PAGE_SIZE), 0);
        CU_ASSERT_EQUAL(do_free(g_phys[i]), 0);
        g_phys[i] = 0;
    }
}

/* Release before reset: pcm_mixer_reset() forgets the ring without freeing it
 * (the mixer does not own it), so the pages must go back first. */
static void quiesce(void)
{
    syscall_sound_release(context_current());
    syscall_sound_release(OTHER_LIBOS);
    hda_pcm_stop();
    pcm_mixer_reset();
}

int syscall_sound_stream_suite_init(void)
{
    if (vmm_create_address_space(&g_other_pml4) != VMM_OK)
        return 1;
    if (context_create(g_other_pml4, &g_other) != CONTEXT_OK)
        return 1;
    quiesce();
    return 0;
}

int syscall_sound_stream_suite_cleanup(void)
{
    quiesce();
    libos_test_teardown_owner(TEST_OWNER_SOUND_STREAM);
    if (g_other != PAGE_OWNER_FREE) {
        context_destroy(g_other);
        g_other = PAGE_OWNER_FREE;
    }
    return 0;
}

/* ── Binding ────────────────────────────────────────────────────────────── */

static void test_handlers_are_bound(void)
{
    CU_ASSERT_PTR_NOT_NULL(exo_syscall_handler(EXO_SYS_SOUND_STREAM_WRITE));
    CU_ASSERT_PTR_NOT_NULL(exo_syscall_handler(EXO_SYS_SOUND_STREAM_STOP));
    CU_ASSERT_PTR_NOT_NULL(exo_syscall_handler(EXO_SYS_SOUND_STREAM_VOLUME));
}

/* ── Argument validation ────────────────────────────────────────────────── */

static void test_rejects_bad_frame_count(void)
{
    CU_ASSERT_TRUE(hda_present());
    map_scratch();

    CU_ASSERT_EQUAL(do_write(SCRATCH, 0), -EXO_EINVAL);
    CU_ASSERT_EQUAL(do_write(SCRATCH, SOUND_STREAM_MAX_FRAMES + 1u),
                    -EXO_EINVAL);
    CU_ASSERT_EQUAL(do_write(SCRATCH, 1ULL << 32), -EXO_EINVAL);

    /* Rejected before anything is bound. */
    CU_ASSERT_EQUAL(syscall_sound_stream_pages_used(), 0u);
    CU_ASSERT_EQUAL(syscall_sound_stream_holder(), PAGE_OWNER_FREE);

    unmap_scratch();
    quiesce();
}

static void test_rejects_buffer_outside_the_window(void)
{
    CU_ASSERT_TRUE(hda_present());

    CU_ASSERT_EQUAL(do_write(0, 16), -EXO_EFAULT);
    CU_ASSERT_EQUAL(do_write(EXO_USER_VA_BASE - 4096, 16), -EXO_EFAULT);
    CU_ASSERT_EQUAL(do_write(EXO_USER_VA_END, 16), -EXO_EFAULT);
    CU_ASSERT_EQUAL(do_write(EXO_USER_VA_END - 8, 16), -EXO_EFAULT);

    CU_ASSERT_EQUAL(syscall_sound_stream_pages_used(), 0u);
}

static void test_rejects_unmapped_and_crossing_buffers(void)
{
    CU_ASSERT_TRUE(hda_present());
    map_scratch();

    CU_ASSERT_EQUAL(do_write(UNMAPPED, 16), -EXO_EFAULT);

    /* Starts mapped, runs off the end of the last scratch page. */
    uint64_t last = SCRATCH + SCRATCH_PAGES * VMM_PAGE_SIZE - 8u;
    CU_ASSERT_EQUAL(do_write(last, 16), -EXO_EFAULT);

    CU_ASSERT_EQUAL(syscall_sound_stream_pages_used(), 0u);
    unmap_scratch();
    quiesce();
}

/* ── Streaming ──────────────────────────────────────────────────────────── */

static void test_write_binds_and_queues(void)
{
    CU_ASSERT_TRUE(hda_present());
    map_scratch();

    int64_t r = do_write(SCRATCH, 256);
    CU_ASSERT_TRUE(r >= 0);
    CU_ASSERT_EQUAL(accepted(r), 256u);
    CU_ASSERT_EQUAL(space(r), PCM_MIXER_MUSIC_FRAMES - 256u);

    CU_ASSERT_EQUAL(syscall_sound_stream_holder(), context_current());
    CU_ASSERT_EQUAL(syscall_sound_stream_pages_used(), PCM_MIXER_MUSIC_PAGES);
    CU_ASSERT_TRUE(pcm_mixer_music_is_open());
    CU_ASSERT_EQUAL(pcm_mixer_music_fill(), 256u);
    CU_ASSERT_TRUE(hda_pcm_is_streaming());

    /* A second write by the holder tops it up; no second ring. */
    r = do_write(SCRATCH, 256);
    CU_ASSERT_EQUAL(accepted(r), 256u);
    CU_ASSERT_EQUAL(pcm_mixer_music_fill(), 512u);
    CU_ASSERT_EQUAL(syscall_sound_stream_pages_used(), PCM_MIXER_MUSIC_PAGES);

    unmap_scratch();
    quiesce();
}

/* The point of the call: a full ring is a short count, then a zero, never a
 * block and never a fault. */
static void test_short_write_when_ring_fills(void)
{
    CU_ASSERT_TRUE(hda_present());
    map_scratch();

    uint32_t total = 0;
    int64_t r = 0;

    /* 1024-frame chunks until the 12288-frame ring is exactly full. */
    for (uint32_t i = 0; i < PCM_MIXER_MUSIC_FRAMES / SCRATCH_FRAMES; i++) {
        r = do_write(SCRATCH, SCRATCH_FRAMES);
        CU_ASSERT_EQUAL(accepted(r), SCRATCH_FRAMES);
        total += accepted(r);
    }
    CU_ASSERT_EQUAL(total, PCM_MIXER_MUSIC_FRAMES);
    CU_ASSERT_EQUAL(space(r), 0u);

    /* Full: nothing taken, not an error. */
    r = do_write(SCRATCH, 64);
    CU_ASSERT_TRUE(r >= 0);
    CU_ASSERT_EQUAL(accepted(r), 0u);
    CU_ASSERT_EQUAL(space(r), 0u);

    /* Make room for 100 frames (render drains the ring) and offer 400. */
    int16_t sink[100 * PCM_MIXER_CHANNELS];
    pcm_mixer_render(sink, 100);
    r = do_write(SCRATCH, 400);
    CU_ASSERT_TRUE(r >= 0);
    CU_ASSERT_EQUAL(accepted(r), 100u);
    CU_ASSERT_EQUAL(space(r), 0u);

    unmap_scratch();
    quiesce();
}

static void test_ring_holds_the_callers_frames(void)
{
    CU_ASSERT_TRUE(hda_present());
    map_scratch();

    CU_ASSERT_EQUAL(accepted(do_write(SCRATCH, 8)), 8u);

    /* Music volume defaults to full, so the first frames out are the first
     * frames in (ramp 1000, 1001, ... per int16, L/R interleaved). */
    int16_t out[8 * PCM_MIXER_CHANNELS];
    pcm_mixer_render(out, 8);
    int match = 1;
    for (uint32_t i = 0; i < 8 * PCM_MIXER_CHANNELS; i++)
        if (out[i] != (int16_t)(1000 + (i & 0xFF)))
            match = 0;
    CU_ASSERT_TRUE(match);

    unmap_scratch();
    quiesce();
}

/* ── Ownership ──────────────────────────────────────────────────────────── */

static void test_second_context_is_busy(void)
{
    CU_ASSERT_TRUE(hda_present());
    page_owner_t me = context_current();
    map_scratch();

    CU_ASSERT_TRUE(do_write(SCRATCH, 64) >= 0);

    context_set_current(OTHER_LIBOS);
    CU_ASSERT_EQUAL(do_write(SCRATCH, 64), -EXO_EBUSY);
    CU_ASSERT_EQUAL(do_stop(), -EXO_EPERM);
    context_set_current(me);

    /* Left exactly as the holder had it. */
    CU_ASSERT_EQUAL(syscall_sound_stream_holder(), me);
    CU_ASSERT_EQUAL(pcm_mixer_music_fill(), 64u);

    unmap_scratch();
    quiesce();
}

static void test_stop_releases_and_frees_pages(void)
{
    CU_ASSERT_TRUE(hda_present());
    page_owner_t me = context_current();
    map_scratch();

    CU_ASSERT_TRUE(do_write(SCRATCH, 64) >= 0);
    CU_ASSERT_EQUAL(do_stop(), 0);

    CU_ASSERT_EQUAL(syscall_sound_stream_pages_used(), 0u);
    CU_ASSERT_EQUAL(syscall_sound_stream_holder(), PAGE_OWNER_FREE);
    CU_ASSERT_FALSE(pcm_mixer_music_is_open());
    CU_ASSERT_EQUAL(pcm_mixer_music_fill(), 0u);

    /* Idempotent, and a different context can now bind. */
    CU_ASSERT_EQUAL(do_stop(), 0);
    context_set_current(OTHER_LIBOS);
    CU_ASSERT_TRUE(do_write(SCRATCH, 64) >= 0);
    CU_ASSERT_EQUAL(syscall_sound_stream_holder(), OTHER_LIBOS);
    context_set_current(me);

    unmap_scratch();
    quiesce();
}

/* exo_exit calls syscall_sound_release(owner) (src/syscall_exit.c): the
 * holder's death must free the ring, not leak the soundtrack. */
static void test_release_frees_only_the_holders_stream(void)
{
    CU_ASSERT_TRUE(hda_present());
    page_owner_t me = context_current();
    map_scratch();

    CU_ASSERT_TRUE(do_write(SCRATCH, 64) >= 0);

    syscall_sound_release(OTHER_LIBOS);       /* not the holder: untouched */
    CU_ASSERT_EQUAL(syscall_sound_stream_holder(), me);
    CU_ASSERT_TRUE(pcm_mixer_music_is_open());

    syscall_sound_release(me);
    CU_ASSERT_EQUAL(syscall_sound_stream_holder(), PAGE_OWNER_FREE);
    CU_ASSERT_EQUAL(syscall_sound_stream_pages_used(), 0u);
    CU_ASSERT_FALSE(pcm_mixer_music_is_open());
    CU_ASSERT_EQUAL(pcm_mixer_music_fill(), 0u);

    unmap_scratch();
    quiesce();
}

/* ── Volume ─────────────────────────────────────────────────────────────── */

static void test_volume_is_unowned_and_clamped(void)
{
    CU_ASSERT_TRUE(hda_present());
    page_owner_t me = context_current();
    map_scratch();

    /* Before any stream exists (Doom sets the level first). */
    CU_ASSERT_EQUAL(do_volume(40), 0);
    CU_ASSERT_EQUAL(pcm_mixer_music_volume(), 40);

    CU_ASSERT_TRUE(do_write(SCRATCH, 8) >= 0);

    /* A non-holder may still set it, and it clamps. */
    context_set_current(OTHER_LIBOS);
    CU_ASSERT_EQUAL(do_volume(1000), 0);
    CU_ASSERT_EQUAL(pcm_mixer_music_volume(), PCM_MIXER_VOL_MAX);
    context_set_current(me);

    CU_ASSERT_EQUAL(do_volume(0), 0);
    CU_ASSERT_EQUAL(pcm_mixer_music_volume(), 0);

    /* Persists across a release. */
    CU_ASSERT_EQUAL(do_stop(), 0);
    CU_ASSERT_EQUAL(pcm_mixer_music_volume(), 0);

    unmap_scratch();
    quiesce();
}

/* ── Ring 3 ─────────────────────────────────────────────────────────────── */

extern void sound_stream_probe(void);
extern void sound_stream_probe_end(void);

/* A real LibOS, a real `syscall`, frames from its own data page. */
static void test_ring3_streams_through_the_syscall(void)
{
    CU_ASSERT_TRUE(hda_present());
    quiesce();

    static int16_t frames[PROBE_FRAMES * PCM_MIXER_CHANNELS];
    for (uint32_t i = 0; i < PROBE_FRAMES * PCM_MIXER_CHANNELS; i++)
        frames[i] = (int16_t)(2000 + (i & 0x7F));

    size_t code_len = (uintptr_t)&sound_stream_probe_end -
                      (uintptr_t)&sound_stream_probe;

    libos_image_t img;
    CU_ASSERT_EQUAL(libos_build_image(TEST_OWNER_SOUND_STREAM,
                                      (const void *)&sound_stream_probe,
                                      code_len, frames, sizeof(frames), 0,
                                      &img),
                    VMM_OK);

    exo_syscall_register(LIBOS_RETURN_SYSCALL_NUM, libos_return);
    CU_ASSERT_EQUAL(vmm_switch_address_space(img.pml4_phys), VMM_OK);
    uint64_t result = libos_enter(img.entry_vaddr, img.stack_top_vaddr);
    CU_ASSERT_EQUAL(vmm_switch_address_space(vmm_kernel_pml4()), VMM_OK);
    exo_syscall_register(LIBOS_RETURN_SYSCALL_NUM, 0);

    int64_t r = (int64_t)result;
    CU_ASSERT_TRUE(r >= 0);
    CU_ASSERT_EQUAL(accepted(r), PROBE_FRAMES);
    CU_ASSERT_EQUAL(space(r), PCM_MIXER_MUSIC_FRAMES - PROBE_FRAMES);

    CU_ASSERT_TRUE(pcm_mixer_music_is_open());
    CU_ASSERT_EQUAL(pcm_mixer_music_fill(), PROBE_FRAMES);
    CU_ASSERT_EQUAL(syscall_sound_stream_pages_used(), PCM_MIXER_MUSIC_PAGES);
    CU_ASSERT_TRUE(hda_pcm_is_streaming());

    /* The probe's own bytes made it into the ring, not zeros. */
    int16_t out[16 * PCM_MIXER_CHANNELS];
    pcm_mixer_render(out, 16);
    int match = 1;
    for (uint32_t i = 0; i < 16 * PCM_MIXER_CHANNELS; i++)
        if (out[i] != frames[i])
            match = 0;
    CU_ASSERT_TRUE(match);

    quiesce();
    CU_ASSERT_EQUAL(syscall_sound_stream_pages_used(), 0u);
    libos_destroy_image(TEST_OWNER_SOUND_STREAM, &img);
}

/* ── Registration ──────────────────────────────────────────────────────── */

void suite_syscall_sound_stream_tests(CU_pSuite s)
{
    CU_add_test(s, "handlers are bound", test_handlers_are_bound);
    CU_add_test(s, "rejects a bad frame count", test_rejects_bad_frame_count);
    CU_add_test(s, "rejects a buffer outside the window",
                test_rejects_buffer_outside_the_window);
    CU_add_test(s, "rejects unmapped and crossing buffers",
                test_rejects_unmapped_and_crossing_buffers);
    CU_add_test(s, "write binds the stream and queues frames",
                test_write_binds_and_queues);
    CU_add_test(s, "a full ring gives a short write, then zero",
                test_short_write_when_ring_fills);
    CU_add_test(s, "the ring holds the caller's frames",
                test_ring_holds_the_callers_frames);
    CU_add_test(s, "a second context is refused with EBUSY",
                test_second_context_is_busy);
    CU_add_test(s, "stop releases the stream and frees its pages",
                test_stop_releases_and_frees_pages);
    CU_add_test(s, "release frees only the holder's stream",
                test_release_frees_only_the_holders_stream);
    CU_add_test(s, "volume is unowned, clamped and persistent",
                test_volume_is_unowned_and_clamped);
    CU_add_test(s, "ring 3 streams through the syscall",
                test_ring3_streams_through_the_syscall);
}
