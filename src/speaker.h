#pragma once
#include <stdint.h>

/*
 * speaker.h — PC speaker driver: PIT channel 2 tone generation (SCRUM-98).
 *
 * The speaker is a one-bit output gated by port 0x61: bit 0 is PIT channel
 * 2's GATE input, bit 1 connects channel 2's OUT pin to the speaker. With
 * channel 2 in mode 3 (square wave) at divisor 1193180 / f, both bits set
 * plays a tone at f Hz. Channel 0 (the system timer, src/pit.c) shares only
 * the mode/command port 0x43 with it, and every write to 0x43 names its own
 * channel, so neither disturbs the other.
 *
 * Duration is the kernel's job, not the caller's: speaker_tone() returns at
 * once and irq0_handler() calls speaker_tick() every tick, which silences
 * the speaker when the deadline passes. That is what lets the exo_sound_tone
 * syscall (SCRUM-100) be non-blocking -- a LibOS never has to sleep through
 * its own sound effect.
 *
 * Ring 0 only. Nothing here checks who is asking; SCRUM-100's syscall layer
 * is the gate in front of it, and ring 3 cannot reach ports 0x42/0x43/0x61
 * itself (IOPL 0, no I/O permission bitmap -- see test_port_io_fault_k.c).
 */

/* The PIT's input clock, as src/pit.c's pit_init() rounds it. */
#define SPEAKER_PIT_BASE_HZ 1193180u

/* Supported tone range. The floor is where the divisor still fits channel
 * 2's 16-bit counter (1193180 / 19 = 62799; 18 Hz would need 66288). The
 * ceiling is the top of human hearing -- a higher tone is inaudible, and
 * accepting it would only let a caller "play" silence. */
#define SPEAKER_MIN_HZ 19u
#define SPEAKER_MAX_HZ 20000u

#define SPEAKER_OK      0
#define SPEAKER_EINVAL -1

/* Silence the speaker and forget any pending deadline. Called once from
 * kernel_main after pit_init(), because firmware can leave port 0x61's gate
 * bits set; harmless to call again. */
void speaker_init(void);

/* Start a tone at freq_hz for dur_ms milliseconds and return immediately.
 * dur_ms == 0 means "until speaker_stop() or the next speaker_tone()". A new
 * tone replaces whatever was playing, deadline included. Returns
 * SPEAKER_EINVAL -- and leaves the speaker exactly as it was -- when freq_hz
 * lies outside [SPEAKER_MIN_HZ, SPEAKER_MAX_HZ]. Safe with IF set or clear. */
int speaker_tone(uint32_t freq_hz, uint32_t dur_ms);

/* Silence the speaker now. Safe with IF set or clear. */
void speaker_stop(void);

/* 1 while a tone is sounding, 0 once stopped or its duration has run out. */
int speaker_is_playing(void);

/* Called from irq0_handler() with the current kernel_get_ticks_ms(): stops
 * a timed tone once now_ms reaches its deadline. Wrap-safe. */
void speaker_tick(uint32_t now_ms);

/* The channel 2 reload value speaker_tone() programs for freq_hz, rounded
 * to nearest the same way pit_init() rounds channel 0's. Only meaningful
 * for freq_hz in the supported range. Exposed for tests. */
uint16_t speaker_divisor_for(uint32_t freq_hz);
