#include "syscall_sound.h"
#include "syscall.h"
#include "exo_syscall.h"
#include "speaker.h"
#include "pcm_mixer.h"
#include "doom_dmx.h"
#include "hda.h"
#include "page_alloc.h"
#include "vmm.h"
#include "string.h"

#include <stddef.h>
#include <stdint.h>

/*
 * syscall_sound.c — the sound syscalls (SCRUM-100).
 *
 * The speaker is one voice, so there is no binding table in the style of
 * fb_binding.c/disk_binding.c: acquiring it up front would only let one
 * LibOS lock every other one out of making a sound at all. The policy is
 * last-tone-wins, and the only ownership kept is who started the tone that
 * is sounding now -- which is what decides whether exo_sound_stop may cut it
 * and whether exo_exit should.
 *
 * No locking: syscalls run with IF cleared by IA32_FMASK, and the one piece
 * of state IRQ0 also touches (the driver's deadline) is the driver's to
 * guard.
 *
 * ── The PCM half (SCRUM-213) ───────────────────────────────────────────
 *
 * #27/#28 put the software mixer (src/pcm_mixer.c, SCRUM-212) and the HDA
 * output stream (src/hda.c, SCRUM-210) behind the same two questions this
 * file already answers for the speaker, and the answers come out the same
 * way for a stronger reason.
 *
 * *Shared, not acquired.*  The ticket asks whether sound should be
 * exclusive-bound like the framebuffer (exo_fb_acquire, -EXO_EBUSY to a
 * second claimant) or shared like the tone path.  Shared -- because
 * pcm_mixer_start() already *is* the admission-control policy: eight voices,
 * Doom's priority rule, and an explicit refusal of a less important sound
 * rather than always stealing.  A binding table on top would answer the same
 * question a second time and worse, since the one thing it can express
 * ("this LibOS has the device") is the one outcome eight voices exist to
 * avoid.  So there is no exo_sound_acquire and no src/sound_binding.c, and
 * the ownership kept is per *voice*, exactly as tone_holder is per tone.
 *
 * *The samples are copied.*  pcm_mixer_start() keeps the pointer it is given
 * for the whole life of the voice and reads through it from
 * hda_irq_handler().  A ring-3 pointer cannot survive that: the IRQ fires
 * under whatever CR3 happens to be loaded, and the caller is free to
 * exo_page_unmap or exo_page_free the buffer the instant the syscall
 * returns -- which would be a fatal supervisor page fault at best
 * (src/fault.c, SCRUM-186) and another context's page played out loud at
 * worst.  So the handler copies into kernel pages, and the voice's lifetime
 * becomes the kernel's to guarantee.  That is the same reasoning
 * src/doom_dmx.h gives for *not* copying in ring 0 -- the IWAD is
 * kernel-mapped and read-only for the life of the mount -- applied where
 * neither of those holds.
 *
 * Those pages are freed when the voice they belong to is no longer playing,
 * swept at the top of each sound syscall and in syscall_sound_release().
 * Deliberately not freed where the voice actually retires: that happens
 * inside hda_irq_handler(), and free_page_owned() there would race the PMM
 * bitmap against ring-0 code that an interrupt can preempt (a syscall body
 * cannot be, having IF clear, but kernel_main and its callees can).  The cost
 * of sweeping late is that a finished effect holds its pages until the next
 * sound syscall, bounded by PCM_MIXER_VOICES buffers.
 */

static page_owner_t tone_holder = PAGE_OWNER_FREE;

/*
 * One row per mixer voice, tracking what this file lent that voice: who
 * started it and the kernel pages its samples were copied into.  `buf ==
 * NULL` means the row is free.  Keyed by nothing -- it is searched linearly,
 * over eight entries, inside a syscall that has already done a memcpy.
 */
typedef struct {
    page_owner_t owner;   /* who called exo_sound_pcm                       */
    int          handle;  /* the mixer's handle, or -1 when the row is free */
    uint8_t     *buf;     /* contiguous kernel pages holding the samples    */
    uint32_t     pages;   /* how many, for the free loop                    */
} pcm_slot_t;

static pcm_slot_t pcm_slots[PCM_MIXER_VOICES];

static uint32_t pages_for(uint32_t bytes)
{
    return (uint32_t)(((uint64_t)bytes + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE);
}

/* Give a staging run back to the PMM.  The run came from
 * alloc_pages_contig_owned(), whose pages are tagged individually and so are
 * freed one at a time (src/page_alloc.h).  Split out from slot_release()
 * because the handler has to undo an allocation that never reached a row. */
static void staging_free(uint8_t *buf, uint32_t pages)
{
    for (uint32_t i = 0; i < pages; i++)
        (void)free_page_owned(buf + (uint64_t)i * VMM_PAGE_SIZE,
                              PAGE_OWNER_KERNEL);
}

/* Return a row's pages and mark it free. */
static void slot_release(pcm_slot_t *slot)
{
    staging_free(slot->buf, slot->pages);

    slot->buf    = NULL;
    slot->pages  = 0;
    slot->handle = -1;
    slot->owner  = PAGE_OWNER_FREE;
}

/* Free the staging buffers of every voice that has stopped sounding -- run
 * before any row is handed out, so a finished effect's pages are available to
 * the next one rather than held until something else happens to call in. */
static void pcm_reap(void)
{
    for (uint32_t i = 0; i < PCM_MIXER_VOICES; i++) {
        pcm_slot_t *slot = &pcm_slots[i];

        if (slot->buf != NULL && !pcm_mixer_is_playing(slot->handle))
            slot_release(slot);
    }
}

static pcm_slot_t *slot_for_handle(int handle)
{
    for (uint32_t i = 0; i < PCM_MIXER_VOICES; i++) {
        if (pcm_slots[i].buf != NULL && pcm_slots[i].handle == handle)
            return &pcm_slots[i];
    }
    return NULL;
}

static pcm_slot_t *slot_free_row(void)
{
    for (uint32_t i = 0; i < PCM_MIXER_VOICES; i++) {
        if (pcm_slots[i].buf == NULL)
            return &pcm_slots[i];
    }
    return NULL;
}

/* #17 — start a tone at `freq` Hz for `dur_ms` ms and return at once.
 *   0              tone started (replacing any tone already sounding)
 *   -EXO_EINVAL    freq outside [SPEAKER_MIN_HZ, SPEAKER_MAX_HZ], or dur_ms
 *                  outside [1, SOUND_TONE_MAX_MS]; the speaker is untouched
 * Backs the Doom sound module's StartSound (docs/syscall_spec.md §3.2 #17). */
static int64_t sys_sound_tone(uint64_t freq, uint64_t dur_ms, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;

    /* Range-check the full 64-bit values before narrowing, or a freq of
     * 2^32 + 440 would truncate to a perfectly valid 440. */
    if (freq < SPEAKER_MIN_HZ || freq > SPEAKER_MAX_HZ)
        return -EXO_EINVAL;
    if (dur_ms == 0 || dur_ms > SOUND_TONE_MAX_MS)
        return -EXO_EINVAL;

    if (speaker_tone((uint32_t)freq, (uint32_t)dur_ms) != SPEAKER_OK)
        return -EXO_EINVAL;

    tone_holder = syscall_current_context();
    return 0;
}

/* #18 — silence the speaker now.
 *   0              silenced, or nothing was sounding
 *   -EXO_EPERM     a different context's tone is sounding; left alone
 * Backs the Doom sound module's StopSound (docs/syscall_spec.md §3.2 #18). */
static int64_t sys_sound_stop(uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (!speaker_is_playing())
        return 0;
    if (tone_holder != syscall_current_context())
        return -EXO_EPERM;

    speaker_stop();
    return 0;
}

/* #27 -- copy `num_samples` bytes of 8-bit unsigned mono PCM from `buf` onto
 * a mixer voice and start it.
 *   >= 0           the voice handle, for exo_sound_pcm_stop
 *   -EXO_EINVAL    num_samples 0 or over SOUND_PCM_MAX_SAMPLES, or a rate the
 *                  mixer cannot play
 *   -EXO_ENODEV    no audio controller, or the output stream refused to start
 *   -EXO_EFAULT    [buf, buf + num_samples) is not entirely inside the LibOS
 *                  window and mapped
 *   -EXO_EBUSY     every voice is busy with sound at least as important; as
 *                  pcm_mixer_start() guarantees, nothing playing was disturbed
 *   -EXO_ENOMEM    no contiguous run of kernel pages that size is free
 * Backs a LibOS sound module's StartSound (docs/syscall_spec.md §3.2 #27).
 *
 * Argument order is shape, then device, then buffer, then resource -- the
 * precedence src/syscall_disk.c settled on: "there is no such device" is a
 * more fundamental answer than "your pointer is bad", and a caller learns
 * nothing about kernel memory from a call it could never have made. */
static int64_t sys_sound_pcm(uint64_t buf, uint64_t num_samples,
                             uint64_t rate_hz, uint64_t vol, uint64_t sep,
                             uint64_t priority)
{
    /* Range-check the untruncated 64-bit values, for the reason
     * sys_sound_tone gives above: a num_samples of 2^32 + 1024 must not
     * narrow into a perfectly valid 1024. */
    if (num_samples == 0 || num_samples > SOUND_PCM_MAX_SAMPLES)
        return -EXO_EINVAL;

    /* The mixer's phase step is rate_hz << 16 in 32 bits, so anything past
     * 65535 Hz overflows it; the low end (a step of 0, a rate under ~0.73 Hz)
     * pcm_mixer_start() rejects itself. Nothing Doom ships comes near either
     * -- the highest rate in freedoom2 is 44100 -- but rate_hz is a caller's
     * number, not a lump's, once it arrives through a syscall. */
    if (rate_hz == 0 || rate_hz > 65535u)
        return -EXO_EINVAL;

    if (!hda_present())
        return -EXO_ENODEV;

    /* Bounds, then presence, exactly as exo_serial_write does it: the window
     * is reserved-but-unmapped until the caller maps it, so an in-window
     * pointer can still be absent, and the memcpy below would take a fatal
     * supervisor page fault on it (SCRUM-186). Read-only: the kernel only
     * reads these samples. */
    if (!exo_range_in_user_window(buf, num_samples) ||
        !exo_user_range_mapped(buf, num_samples, 0))
        return -EXO_EFAULT;

    pcm_reap();

    uint32_t pages = pages_for((uint32_t)num_samples);
    uint8_t *staging = (uint8_t *)alloc_pages_contig_owned(PAGE_OWNER_KERNEL,
                                                           pages);
    if (staging == NULL)
        return -EXO_ENOMEM;

    memcpy(staging, (const void *)(uintptr_t)buf, (size_t)num_samples);

    /* Started lazily rather than from syscall_sound_init(): a stream running
     * with every voice silent still costs ~47 interrupts a second forever
     * (docs/drivers/hda.md §12), and on most boots nobody ever asks for a
     * sound. hda_pcm_start() is idempotent while streaming, but checking
     * first keeps the boot tone's own mode alone until there is really
     * something to play. */
    if (!hda_pcm_is_streaming() && hda_pcm_start() != HDA_OK) {
        staging_free(staging, pages);
        return -EXO_ENODEV;
    }

    /* The mixer takes a doom_dmx_t because that is the shape of everything it
     * plays; a local is enough, since pcm_mixer_start() copies the fields it
     * needs and keeps only the sample pointer (src/pcm_mixer.h). */
    doom_dmx_t pcm = {
        .format      = DOOM_DMX_FORMAT_PCM,
        .rate_hz     = (uint32_t)rate_hz,
        .samples     = staging,
        .num_samples = (uint32_t)num_samples,
    };

    int handle = pcm_mixer_start(&pcm, (int)(int64_t)vol, (int)(int64_t)sep,
                                 (int)(int64_t)priority);
    if (handle < 0) {
        staging_free(staging, pages);
        return handle == PCM_MIXER_ENOVOICE ? -EXO_EBUSY : -EXO_EINVAL;
    }

    /* A row is claimed *after* the mixer has spoken, not before -- and the
     * sweep is repeated here, because those are the same statement.  With all
     * eight voices busy, pcm_mixer_start() is entitled to take one over for a
     * more important sound, and the handle of the voice it took is stale from
     * that moment: it is the second pcm_reap() that notices, frees that
     * sound's staging pages and leaves the row this one needs.  Refusing on a
     * full slot table *before* the call would have pre-empted the mixer's own
     * admission decision with a worse one -- the bug the ninth-voice test
     * caught. */
    pcm_reap();

    pcm_slot_t *slot = slot_free_row();
    if (slot == NULL) {
        /* Unreachable: the mixer has just told us it has a voice for this
         * sound, and the sweep above frees the row of whatever it displaced.
         * Handled rather than asserted, since the alternative to giving the
         * sound up here is leaking a staging run. */
        pcm_mixer_stop(handle);
        staging_free(staging, pages);
        return -EXO_EBUSY;
    }

    slot->owner  = syscall_current_context();
    slot->handle = handle;
    slot->buf    = staging;
    slot->pages  = pages;
    return (int64_t)handle;
}

/* #28 -- stop the voice `handle` names.
 *   0              stopped; also when the sound has already finished or been
 *                  taken over by a more important one, since there is then
 *                  nothing left to stop and no error to report
 *   -EXO_EINVAL    a negative handle, which was never issued
 *   -EXO_EPERM     that voice is playing another context's sound
 * Backs a LibOS sound module's StopSound (docs/syscall_spec.md §3.2 #28). */
static int64_t sys_sound_pcm_stop(uint64_t handle, uint64_t a2, uint64_t a3,
                                  uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if ((int64_t)handle < 0)
        return -EXO_EINVAL;

    /* Before the lookup, so a handle whose sound finished on its own is a row
     * that is already gone -- "nothing to stop", rather than a row whose
     * owner has to be checked to say the same thing. */
    pcm_reap();

    pcm_slot_t *slot = slot_for_handle((int)(int64_t)handle);
    if (slot == NULL)
        return 0;

    if (slot->owner != syscall_current_context())
        return -EXO_EPERM;

    pcm_mixer_stop(slot->handle);
    slot_release(slot);
    return 0;
}

uint32_t syscall_sound_pcm_slots_used(void)
{
    uint32_t used = 0;

    for (uint32_t i = 0; i < PCM_MIXER_VOICES; i++)
        if (pcm_slots[i].buf != NULL)
            used++;

    return used;
}

uint32_t syscall_sound_pcm_pages_used(void)
{
    uint32_t pages = 0;

    for (uint32_t i = 0; i < PCM_MIXER_VOICES; i++)
        pages += pcm_slots[i].pages;

    return pages;
}

void syscall_sound_release(page_owner_t owner)
{
    if (speaker_is_playing() && tone_holder == owner)
        speaker_stop();

    /* Reap first: a voice of `owner`'s that has already finished needs its
     * pages back just as much, and takes no pcm_mixer_stop() to get there. */
    pcm_reap();

    for (uint32_t i = 0; i < PCM_MIXER_VOICES; i++) {
        pcm_slot_t *slot = &pcm_slots[i];

        if (slot->buf != NULL && slot->owner == owner) {
            pcm_mixer_stop(slot->handle);
            slot_release(slot);
        }
    }
}

void syscall_sound_init(void)
{
    tone_holder = PAGE_OWNER_FREE;

    for (uint32_t i = 0; i < PCM_MIXER_VOICES; i++) {
        pcm_slots[i].owner  = PAGE_OWNER_FREE;
        pcm_slots[i].handle = -1;
        pcm_slots[i].buf    = NULL;
        pcm_slots[i].pages  = 0;
    }

    exo_syscall_register(EXO_SYS_SOUND_TONE, sys_sound_tone);
    exo_syscall_register(EXO_SYS_SOUND_STOP, sys_sound_stop);
    exo_syscall_register(EXO_SYS_SOUND_PCM, sys_sound_pcm);
    exo_syscall_register(EXO_SYS_SOUND_PCM_STOP, sys_sound_pcm_stop);
}
