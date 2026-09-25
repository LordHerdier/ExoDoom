#include "speaker.h"
#include "io.h"
#include "pit.h"

#define PIT_CH2_DATA   0x42
#define PIT_COMMAND    0x43
#define SPEAKER_PORT   0x61

/* 0x61 bit 0: channel 2 GATE; bit 1: OUT -> speaker. Both on = tone. */
#define SPEAKER_GATE_BITS 0x03

/* Channel 2, access lobyte/hibyte, mode 3 (square wave), binary. */
#define PIT_CH2_SQUARE_WAVE 0xB6

static volatile uint8_t  playing;
static volatile uint8_t  timed;
static volatile uint32_t deadline_ms;

/* speaker_tone()/speaker_stop() are called both from syscall context (IF
 * already clear, FMASK) and from ring-0 code that may have IF set; IRQ0's
 * speaker_tick() must not see a half-programmed state or stop a tone whose
 * deadline was written a moment ago. Save and restore rather than sti, so
 * a caller that had IF clear keeps it that way. */
static inline uint64_t irq_save(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags)
{
    __asm__ volatile ("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
}

static void gate_off(void)
{
    outb(SPEAKER_PORT, inb(SPEAKER_PORT) & (uint8_t)~SPEAKER_GATE_BITS);
    playing = 0;
    timed = 0;
}

uint16_t speaker_divisor_for(uint32_t freq_hz)
{
    if (freq_hz == 0) return 0;
    return (uint16_t)((SPEAKER_PIT_BASE_HZ + freq_hz / 2) / freq_hz);
}

void speaker_init(void)
{
    speaker_stop();
}

int speaker_tone(uint32_t freq_hz, uint32_t dur_ms)
{
    if (freq_hz < SPEAKER_MIN_HZ || freq_hz > SPEAKER_MAX_HZ)
        return SPEAKER_EINVAL;

    uint16_t divisor = speaker_divisor_for(freq_hz);
    uint64_t flags = irq_save();

    outb(PIT_COMMAND, PIT_CH2_SQUARE_WAVE);
    outb(PIT_CH2_DATA, (uint8_t)(divisor & 0xFF));
    outb(PIT_CH2_DATA, (uint8_t)(divisor >> 8));
    outb(SPEAKER_PORT, inb(SPEAKER_PORT) | SPEAKER_GATE_BITS);

    timed = (dur_ms != 0);
    deadline_ms = kernel_get_ticks_ms() + dur_ms;
    playing = 1;

    irq_restore(flags);
    return SPEAKER_OK;
}

void speaker_stop(void)
{
    uint64_t flags = irq_save();
    gate_off();
    irq_restore(flags);
}

int speaker_is_playing(void)
{
    return playing;
}

void speaker_tick(uint32_t now_ms)
{
    /* Runs inside irq0_handler() with IF clear, so nothing can interleave. */
    if (playing && timed && (int32_t)(now_ms - deadline_ms) >= 0)
        gate_off();
}
