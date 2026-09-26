#pragma once
#include <stdint.h>

/*
 * hda — Intel High Definition Audio controller driver (SCRUM-210).
 *
 * Ring 0 only, and syscall-free, the same relationship src/speaker.c,
 * src/ata.c and src/pci.c have to their hardware: nothing here knows who is
 * asking. A LibOS-facing exo_sound_pcm and an HDA ownership binding are
 * separate, explicitly-blocked tickets.
 *
 * Why HDA rather than SB16 or AC'97: it is the audio controller that still
 * exists on real x86 machines, and QEMU emulates it as `-device intel-hda`,
 * so the same driver is testable in CI and bootable on metal. It is also the
 * only one of the three that masters its own DMA -- there is no 8237-style
 * DMA controller to build first.
 *
 * Three pieces of hardware, in the order this file brings them up:
 *
 *   1. The *controller*: a PCI function (class 0x0403) whose BAR 0 is a
 *      memory-mapped register block. src/pci.c (SCRUM-209) finds it, sizes
 *      the BAR, enables bus mastering and identity-maps the window PCD.
 *   2. The *codec*: a separate chip on the HDA link, reached only by sending
 *      4-byte "verbs" through the controller. Commands go out through the
 *      CORB (Command Output Ring Buffer) and responses come back through the
 *      RIRB (Response Input Ring Buffer) -- two DMA rings in ordinary system
 *      memory that the controller reads and writes. Walking the codec's
 *      widget tree is how a DAC and an output pin are found, unmuted, and
 *      told what sample format to expect.
 *   3. The *stream*: an output stream descriptor in the controller's
 *      register block, pointed at a BDL (Buffer Descriptor List) -- an array
 *      of {address, length} entries naming the sample buffer in system
 *      memory. The controller DMAs from it and raises an interrupt on each
 *      entry that has IOC set.
 *
 * Everything the controller touches by DMA (CORB, RIRB, BDL, samples) comes
 * from alloc_pages_contig_owned(), never kmalloc(): the bump allocator is
 * finished once page_alloc_init() has run (see CLAUDE.md), and the pages
 * must be physically contiguous because the BDL names physical addresses.
 * The kernel map is an identity map, so the pointer we write through *is*
 * the address the controller is given.
 *
 * The API is deliberately shaped like src/speaker.h's: hda_play_tone()
 * returns immediately and hda_tick(), called from irq0_handler(), ends a
 * timed tone. That is what keeps a future exo_sound_pcm from having to make
 * a LibOS sleep through its own audio.
 */

/* ── Global register offsets (HDA 1.0a §3.3), relative to BAR 0 ────────── */
#define HDA_REG_GCAP        0x00  /* capabilities: OSS/ISS/BSS counts, 64OK */
#define HDA_REG_VMIN        0x02
#define HDA_REG_VMAJ        0x03
#define HDA_REG_GCTL        0x08  /* bit 0 CRST: 0 = in reset, 1 = running   */
#define HDA_REG_WAKEEN      0x0C
#define HDA_REG_STATESTS    0x0E  /* one bit per codec address that answered */
#define HDA_REG_INTCTL      0x20  /* bit 31 GIE, bit 30 CIE, bits n = SIE    */
#define HDA_REG_INTSTS      0x24  /* bit 31 GIS, bits n = stream interrupt   */
#define HDA_REG_CORBLBASE   0x40
#define HDA_REG_CORBUBASE   0x44
#define HDA_REG_CORBWP      0x48  /* 16-bit: software's write pointer        */
#define HDA_REG_CORBRP      0x4A  /* 16-bit: bit 15 RST                      */
#define HDA_REG_CORBCTL     0x4C  /* 8-bit:  bit 1 RUN, bit 0 CMEIE          */
#define HDA_REG_CORBSTS     0x4D
#define HDA_REG_CORBSIZE    0x4E  /* 8-bit:  bits 1:0 size, 7:4 capability   */
#define HDA_REG_RIRBLBASE   0x50
#define HDA_REG_RIRBUBASE   0x54
#define HDA_REG_RIRBWP      0x58  /* 16-bit: controller's WP; bit 15 RST     */
#define HDA_REG_RINTCNT     0x5A
#define HDA_REG_RIRBCTL     0x5C  /* 8-bit:  bit 1 DMAEN, bit 0 RINTCTL      */
#define HDA_REG_RIRBSTS     0x5D
#define HDA_REG_RIRBSIZE    0x5E
#define HDA_REG_DPLBASE     0x70
#define HDA_REG_DPUBASE     0x74

#define HDA_GCTL_CRST       (1u << 0)
#define HDA_CORBRP_RST      (1u << 15)
#define HDA_RIRBWP_RST      (1u << 15)
#define HDA_CORBCTL_RUN     (1u << 1)
#define HDA_RIRBCTL_DMAEN   (1u << 1)
#define HDA_INTCTL_GIE      (1u << 31)
#define HDA_INTSTS_GIS      (1u << 31)

/* CORBSIZE/RIRBSIZE bits 1:0 select 2/16/256 entries; 0x2 is 256, which is
 * the only size every controller must support and the one QEMU implements. */
#define HDA_RINGSIZE_256    0x2
#define HDA_RING_ENTRIES    256

/* GCAP bit fields: bits 15:12 output streams, 11:8 input streams. */
#define HDA_GCAP_OSS(g)     (((g) >> 12) & 0xF)
#define HDA_GCAP_ISS(g)     (((g) >>  8) & 0xF)

/* ── Stream descriptors (HDA 1.0a §3.3.35) ─────────────────────────────── */
/* Descriptor n starts at 0x80 + 0x20*n. Input streams occupy the first
 * GCAP.ISS slots and output streams follow them, so the first *output*
 * descriptor index is GCAP.ISS -- read at runtime, never hardcoded. */
#define HDA_SD_BASE(n)      (0x80u + 0x20u * (uint32_t)(n))
#define HDA_SD_CTL          0x00  /* 3 bytes of control; see HDA_SD_STS below  */
#define HDA_SD_STS          0x03  /* 8-bit status, write-1-to-clear           */
#define HDA_SD_LPIB         0x04  /* link position in buffer, read-only      */
#define HDA_SD_CBL          0x08  /* cyclic buffer length, bytes             */
#define HDA_SD_LVI          0x0C  /* 16-bit: last valid BDL index            */
#define HDA_SD_FIFOS        0x10
#define HDA_SD_FMT          0x12  /* 16-bit sample format                    */
#define HDA_SD_BDPL         0x18
#define HDA_SD_BDPU         0x1C

/*
 * SDnCTL is three bytes and SDnSTS is the fourth, and they share one 32-bit
 * location: a dword read at offset 0x00 returns the control bits in 23:0 and
 * the status byte in 31:24. That makes the status conveniently *readable*
 * through the CTL dword -- and completely unwritable through it. The status
 * byte is its own write-1-to-clear register at offset 0x03, and only an
 * access to 0x03 can clear a bit in it; a dword write to 0x00 carrying that
 * bit is silently dropped, because the control register's writable mask does
 * not cover 31:24 (QEMU models this exactly, with a separate shifted
 * register entry for STS -- hw/audio/intel-hda.c's regtab).
 *
 * Getting this wrong is not a cosmetic bug: SDnSTS.BCIS is what holds the
 * controller's INTx line asserted after a buffer completion, and INTx is
 * level-triggered. A clear that does not clear leaves the line high, so the
 * handler is re-entered the instant it returns, forever. So: write the
 * control word as a dword at HDA_SD_CTL, and read *and clear* the status as
 * a byte at HDA_SD_STS.
 */
#define HDA_SDSTS_SHIFT     24   /* status byte's position in the CTL dword */

#define HDA_SDCTL_SRST      (1u << 0)
#define HDA_SDCTL_RUN       (1u << 1)
#define HDA_SDCTL_IOCE      (1u << 2)
#define HDA_SDCTL_STRIPE_MASK 0x30000u
#define HDA_SDCTL_TAG_SHIFT 20

#define HDA_SDSTS_BCIS      (1u << 2)  /* buffer completion (IOC fired)      */
#define HDA_SDSTS_FIFOE     (1u << 3)  /* FIFO error: DMA fell behind        */
#define HDA_SDSTS_DESE      (1u << 4)  /* descriptor error: bad BDL entry    */
#define HDA_SDSTS_FIFORDY   (1u << 5)

/*
 * SDnFMT for 48 kHz, 16-bit, 2 channels: base 48 kHz (bit 14 = 0), no
 * multiplier or divisor, bits-per-sample 0b001 in bits 6:4, channel count
 * minus one in bits 3:0. This is the one format QEMU's hda-output codec
 * accepts, and it is the HDA link's native rate, so nothing resamples.
 */
#define HDA_FMT_48K_16BIT_STEREO 0x0011
#define HDA_SAMPLE_RATE_HZ  48000u
#define HDA_CHANNELS        2u
#define HDA_BYTES_PER_FRAME 4u        /* 2 channels x 16 bits               */

/* The stream tag the DAC is bound to. Any value 1-15 works; 0 means "not
 * assigned" and would make the codec ignore the stream entirely. */
#define HDA_STREAM_TAG      1

/* ── Codec verbs (HDA 1.0a §7.3) ───────────────────────────────────────── */
#define HDA_VERB_GET_PARAMETER            0xF0000
#define HDA_VERB_SET_CONVERTER_FORMAT     0x20000  /* 16-bit payload         */
#define HDA_VERB_SET_AMP_GAIN_MUTE        0x30000  /* 16-bit payload         */
#define HDA_VERB_GET_AMP_GAIN_MUTE        0xB0000  /* 8-bit payload          */
/* GET_AMP_GAIN_MUTE's payload selects which amp and which channel to report,
 * and uses different bits from the SET form's: bit 7 output (vs. input),
 * bit 5 left (vs. right). The response carries the gain in bits 6:0 and the
 * mute flag in bit 7. */
#define HDA_AMP_GET_OUTPUT_LEFT           0xA0
#define HDA_AMP_GET_MUTE                  (1u << 7)
#define HDA_AMP_GET_GAIN(resp)            ((resp) & 0x7F)
#define HDA_VERB_SET_STREAM_CHANNEL       0x70600
#define HDA_VERB_SET_PIN_WIDGET_CONTROL   0x70700
#define HDA_VERB_SET_EAPD_BTL             0x70C00
#define HDA_VERB_SET_POWER_STATE          0x70500

/* GET_PARAMETER parameter ids. */
#define HDA_PARAM_VENDOR_ID     0x00
#define HDA_PARAM_NODE_COUNT    0x04  /* 23:16 start node, 7:0 count         */
#define HDA_PARAM_FUNC_TYPE     0x05  /* 7:0 function group type             */
#define HDA_PARAM_AUDIO_WIDGET_CAP 0x09
#define HDA_PARAM_PIN_CAP       0x0C
#define HDA_PARAM_OUT_AMP_CAP   0x12

/*
 * OUT_AMP_CAP's fields, which are easy to mix up and were, once: the offset
 * (0 dB point) is bits 6:0, the *number of gain steps* is bits 14:8, and the
 * size of each step is bits 22:16. The maximum gain index is the step count,
 * so that is what SET_AMP_GAIN_MUTE wants -- reading bits 22:16 instead
 * yields the step size, which on QEMU's codec is 3 out of 74 steps and plays
 * the tone about 28 dB down: audible on a scope, inaudible on a speaker.
 */
#define HDA_AMPCAP_NUM_STEPS(c) (((c) >> 8) & 0x7F)

#define HDA_FUNC_TYPE_AUDIO     0x01

#define HDA_WIDGET_TYPE(caps)   (((caps) >> 20) & 0xF)
#define HDA_WIDGET_AUDIO_OUT    0x0
#define HDA_WIDGET_PIN_COMPLEX  0x4
#define HDA_WIDGET_CAP_EAPD     (1u << 16)

#define HDA_PINCAP_OUTPUT       (1u << 4)
#define HDA_PIN_CTL_OUT_ENABLE  0x40

/* ── Tone generation ───────────────────────────────────────────────────── */
/* 16 pages = 64 KiB = 16384 stereo frames = ~341 ms at 48 kHz. The BDL
 * loops, so this is the sustain buffer, not the whole tone. */
#define HDA_BUF_PAGES       16u
#define HDA_BUF_BYTES       (HDA_BUF_PAGES * 4096u)
#define HDA_BUF_FRAMES      (HDA_BUF_BYTES / HDA_BYTES_PER_FRAME)

/* The BDL is split in two so that LVI >= 1, which the spec requires (a
 * single-entry list is illegal), and so a completion interrupt arrives twice
 * per loop rather than once. */
#define HDA_BDL_ENTRIES     2u

/* Peak amplitude of the square wave, out of 32767. A quarter scale is loud
 * enough to hear and quiet enough not to hurt on headphones. */
#define HDA_TONE_AMPLITUDE  8192

/* Same audible range src/speaker.h uses, minus the PIT divisor floor that
 * does not apply here. */
#define HDA_MIN_HZ          20u
#define HDA_MAX_HZ          20000u

/* The tone kernel_main plays once on a normal boot, as the audible half of
 * SCRUM-210's acceptance. 440 Hz because it is unmistakably a deliberate
 * note rather than a fault beep, and 600 ms because that is long enough to
 * hear as a tone (and to cross a BDL entry boundary, so the completion
 * interrupt fires on a real boot too) while being short enough not to talk
 * over the rest of the boot. */
#define HDA_BOOT_TONE_HZ    440u
#define HDA_BOOT_TONE_MS    600u

/* ── Status codes ──────────────────────────────────────────────────────── */
#define HDA_OK           0
#define HDA_ENODEV     (-1)   /* no class-0x0403 function on the bus        */
#define HDA_ENOCODEC   (-2)   /* controller came out of reset, nobody home  */
#define HDA_ETIMEDOUT  (-3)   /* a register poll or verb round trip gave up */
#define HDA_ENOMEM     (-4)   /* PMM had no contiguous pages for DMA        */
#define HDA_EINVAL     (-5)
#define HDA_ENOIRQ     (-6)   /* Interrupt Line unrouted or unusable        */
#define HDA_EMAP       (-7)   /* pci_bar_map() refused the BAR window       */
#define HDA_ENOOUT     (-8)   /* codec exposes no output converter          */

/*
 * Bring the controller up: find it on the PCI bus, map BAR 0, reset, start
 * the CORB/RIRB rings, walk the codec for a DAC and an output pin, allocate
 * the DMA buffers, program the output stream descriptor, and wire the
 * controller's INTx line to an IDT vector.
 *
 * No tone is started -- hda_play_tone() does that. Returns HDA_OK, or one of
 * the codes above with everything it managed to set up left in place and a
 * diagnostic on serial. Never halts and never blocks indefinitely: every
 * poll loop is bounded, because this runs on the boot path and a wedged
 * register must not hang a CI boot.
 *
 * Idempotent in the sense pci_init() is: calling it again re-runs the whole
 * sequence against the already-allocated buffers.
 */
int hda_init(void);

/* 1 once hda_init() has found and mapped a controller, 0 otherwise. Every
 * accessor below reads 0 when this is 0 rather than dereferencing a null
 * BAR. */
int hda_present(void);

/*
 * Start a square-wave tone at (approximately) freq_hz and return at once.
 * dur_ms == 0 means "until hda_stop()". The actual frequency is snapped so
 * that a whole number of periods fits the buffer -- the BDL loops, and a
 * partial period at the wrap point is an audible click. hda_tone_hz()
 * reports what was actually programmed.
 *
 * Returns HDA_EINVAL -- leaving the stream untouched -- for a frequency
 * outside [HDA_MIN_HZ, HDA_MAX_HZ], or HDA_ENODEV if hda_init() did not
 * complete. Safe with IF set or clear.
 */
int hda_play_tone(uint32_t freq_hz, uint32_t dur_ms);

/* Stop the stream now (clears SDnCTL.RUN). Safe with IF set or clear. */
void hda_stop(void);

/* 1 while the stream is running. */
int hda_is_playing(void);

/* The frequency hda_play_tone() actually programmed, after snapping. */
uint32_t hda_tone_hz(void);

/* Called from irq0_handler() with kernel_get_ticks_ms(): ends a timed tone
 * once its deadline passes. Wrap-safe, exactly like speaker_tick(). */
void hda_tick(uint32_t now_ms);

/* The controller's own interrupt handler, called from irq_hda_stub. */
void hda_irq_handler(void);

/*
 * Send one verb to `codec`.`node` and wait for its response. `verb` is a
 * pre-shifted HDA_VERB_* constant OR'd with its payload. *resp receives the
 * 32-bit response (it may be NULL for a verb with no useful one). Returns
 * HDA_OK or HDA_ETIMEDOUT. Polls the RIRB rather than taking an interrupt:
 * a verb round trip is synchronous by nature, and this is called from
 * hda_init() with IF possibly clear.
 */
int hda_codec_verb(uint8_t codec, uint8_t node, uint32_t verb, uint32_t *resp);

/* ── Introspection, for the KUnit suite and for debugging ──────────────── */
uint32_t hda_reg_read32(uint32_t off);
uint16_t hda_reg_read16(uint32_t off);
uint8_t  hda_reg_read8(uint32_t off);

/* Read a register of the output stream descriptor this driver programmed,
 * `off` being one of the HDA_SD_* offsets. */
uint32_t hda_sd_read32(uint32_t off);
uint16_t hda_sd_read16(uint32_t off);

/* SDnSTS, extracted from the CTL dword's top byte (see HDA_SDSTS_SHIFT). */
uint8_t  hda_sd_status(void);

uint64_t hda_bar_base(void);        /* BAR 0's physical/virtual base        */
uint8_t  hda_codec_addr(void);      /* the codec address that answered      */
uint8_t  hda_dac_node(void);        /* the output converter widget, 0 = none */
uint8_t  hda_pin_node(void);        /* the output pin widget, 0 = none      */
uint8_t  hda_stream_index(void);    /* descriptor index = GCAP.ISS          */
uint8_t  hda_irq_line(void);        /* 8259 IRQ from PCI Interrupt Line     */
uint64_t hda_corb_phys(void);
uint64_t hda_rirb_phys(void);
uint64_t hda_bdl_phys(void);
uint64_t hda_buf_phys(void);
uint32_t hda_stream_position(void); /* SDnLPIB                              */
uint32_t hda_irq_count(void);       /* completion interrupts seen so far    */
uint8_t  hda_stream_errors(void);   /* latched FIFOE/DESE bits from SDnSTS  */

/* Print the controller/codec/stream state on serial. Called from kernel_main
 * after hda_init(); exposed for debugging. */
void hda_dump(void);
