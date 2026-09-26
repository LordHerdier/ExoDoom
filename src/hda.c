#include "hda.h"
#include "pci.h"
#include "idt.h"
#include "pic.h"
#include "pit.h"
#include "io.h"
#include "serial.h"
#include "page_alloc.h"
#include "vmm.h"
#include "pcm_mixer.h"

#include <stddef.h>

/* src/isr.s — the vector wrapper that calls hda_irq_handler(). */
extern void irq_hda_stub(void);

/*
 * One BDL entry (HDA 1.0a §3.6.2): a physical address, a byte count, and a
 * flags word whose bit 0 asks for an interrupt when the controller finishes
 * that entry. Packed and 16 bytes by definition of the hardware layout, not
 * by preference -- the controller reads this array directly.
 */
typedef struct {
    uint64_t addr;
    uint32_t len;
    uint32_t flags;
} __attribute__((packed)) hda_bdl_entry_t;

#define HDA_BDL_IOC (1u << 0)

/*
 * Upper bound on every register poll in this file. An io_wait() is a write
 * to port 0x80, nominally ~1 us, so this is on the order of a tenth of a
 * second -- far longer than any of these handshakes legitimately takes, and
 * still bounded. hda_init() runs on the boot path: a controller that never
 * answers must cost a diagnostic line, not a hung CI boot.
 */
#define HDA_POLL_TRIES 100000u

static volatile uint8_t  *g_bar;         /* BAR 0, identity-mapped, PCD     */
static uint64_t           g_bar_base;
static const pci_device_t *g_dev;

static int      g_present;
static uint8_t  g_codec = 0xFF;          /* 0xFF = none found               */
static uint8_t  g_afg;                   /* audio function group node       */
static uint8_t  g_dac;                   /* output converter widget         */
static uint8_t  g_pin;                   /* output pin complex widget       */
static uint8_t  g_stream;                /* output stream descriptor index  */
static uint8_t  g_irq_line = 0xFF;

static uint32_t *g_corb;                 /* HDA_RING_ENTRIES dwords         */
static uint32_t *g_rirb;                 /* HDA_RING_ENTRIES x 2 dwords     */
static hda_bdl_entry_t *g_bdl;
static int16_t  *g_buf;

static uint16_t  g_corb_wp;
static uint16_t  g_rirb_rp;

static volatile uint32_t g_irq_count;
static volatile uint8_t  g_stream_err;

static volatile uint8_t  g_playing;
static volatile uint8_t  g_timed;
static volatile uint32_t g_deadline_ms;
static uint32_t          g_tone_hz;

/*
 * Streaming (PCM) mode, SCRUM-212. g_bdl_entries is how finely the buffer is
 * currently diced -- HDA_BDL_ENTRIES for a tone, HDA_BDL_ENTRIES_PCM for a
 * mixed stream -- and is what stream_program() programs LVI from, so each
 * mode's own start path sets it and neither has to undo the other's.
 *
 * g_pcm_write is the refill cursor: the next entry hda_irq_handler() will
 * render into. It trails the DMA engine's position and catches up rather than
 * assuming one interrupt per entry, so a coalesced or missed completion
 * costs a longer render on the next one instead of a permanent gap.
 */
static uint32_t          g_bdl_entries = HDA_BDL_ENTRIES;
static volatile uint8_t  g_streaming;
static volatile uint32_t g_pcm_write;
static volatile uint32_t g_pcm_refills;
static volatile uint32_t g_pcm_underruns;

/* ── MMIO access ───────────────────────────────────────────────────────── */

static inline uint8_t  mmio_r8 (uint32_t o) { return *(volatile uint8_t  *)(g_bar + o); }
static inline uint16_t mmio_r16(uint32_t o) { return *(volatile uint16_t *)(g_bar + o); }
static inline uint32_t mmio_r32(uint32_t o) { return *(volatile uint32_t *)(g_bar + o); }
static inline void mmio_w8 (uint32_t o, uint8_t  v) { *(volatile uint8_t  *)(g_bar + o) = v; }
static inline void mmio_w16(uint32_t o, uint16_t v) { *(volatile uint16_t *)(g_bar + o) = v; }
static inline void mmio_w32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(g_bar + o) = v; }

/* Offset of `off` within this driver's output stream descriptor. */
static inline uint32_t sd(uint32_t off) { return HDA_SD_BASE(g_stream) + off; }

/*
 * Approximate microsecond delay. There is no calibrated microsecond clock in
 * this kernel -- the PIT runs at 1 kHz and kernel_get_ticks_ms() is too
 * coarse for the sub-millisecond waits HDA's reset sequence specifies -- so
 * this counts port-0x80 writes, the same ~1 us primitive src/pic.c already
 * relies on. It always waits *at least* the requested time on QEMU, which is
 * the direction that matters for a reset handshake.
 */
static void delay_us(uint32_t us)
{
    while (us--) {
        io_wait();
    }
}

static uint32_t reg_read_w(int width, uint32_t off)
{
    switch (width) {
    case 1:  return mmio_r8(off);
    case 2:  return mmio_r16(off);
    default: return mmio_r32(off);
    }
}

static int poll_reg(int width, uint32_t off, uint32_t mask, uint32_t want)
{
    for (uint32_t i = 0; i < HDA_POLL_TRIES; i++) {
        if ((reg_read_w(width, off) & mask) == want) {
            return HDA_OK;
        }
        io_wait();
    }
    return HDA_ETIMEDOUT;
}

/* speaker.c's idiom: save/restore rather than sti, so a caller that had IF
 * clear (a syscall, an ISR) keeps it that way. */
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

/* ── Controller discovery and reset ────────────────────────────────────── */

static int controller_find(void)
{
    g_dev = pci_find_class(PCI_CLASS_MULTIMEDIA, PCI_SUBCLASS_AUDIO_DEV);
    if (g_dev == NULL) {
        return HDA_ENODEV;
    }

    pci_bar_t bar;
    if (pci_bar_read(g_dev, 0, &bar) != PCI_OK || bar.is_io || bar.size == 0) {
        return HDA_ENODEV;
    }

    /* Firmware normally programs the BAR before GRUB runs; assign one only
     * if it did not. pci_bar_assign() updates *bar on success. */
    if (bar.base == 0 && pci_bar_assign(g_dev, 0, &bar) != PCI_OK) {
        return HDA_EMAP;
    }

    /* Bus mastering first: HDA drives its own DMA off the BDL, so without
     * it the stream descriptor runs and reads nothing. pci_enable_device()
     * sets it along with the memory decoder. */
    pci_enable_device(g_dev);

    if (pci_bar_map(&bar) != PCI_OK) {
        return HDA_EMAP;
    }

    g_bar_base = bar.base;
    g_bar = (volatile uint8_t *)(uintptr_t)bar.base;
    return HDA_OK;
}

static int controller_reset(void)
{
    /* Quiesce anything a previous hda_init() (or firmware) left running
     * before yanking CRST, so no DMA engine is mid-transfer across it. */
    mmio_w32(HDA_REG_INTCTL, 0);
    mmio_w8(HDA_REG_CORBCTL, 0);
    mmio_w8(HDA_REG_RIRBCTL, 0);

    mmio_w32(HDA_REG_GCTL, mmio_r32(HDA_REG_GCTL) & ~HDA_GCTL_CRST);
    if (poll_reg(4, HDA_REG_GCTL, HDA_GCTL_CRST, 0) != HDA_OK) {
        return HDA_ETIMEDOUT;
    }
    delay_us(100);

    mmio_w32(HDA_REG_GCTL, HDA_GCTL_CRST);
    if (poll_reg(4, HDA_REG_GCTL, HDA_GCTL_CRST, HDA_GCTL_CRST) != HDA_OK) {
        return HDA_ETIMEDOUT;
    }

    /* The spec gives codecs 25 frames (521 us) to report themselves in
     * STATESTS after CRST rises. Reading it earlier finds an empty link on
     * hardware that is merely slow, which is indistinguishable from having
     * no codec at all -- so this wait is correctness, not politeness. */
    delay_us(1000);
    return HDA_OK;
}

static int codec_detect(void)
{
    /* STATESTS is a bitmap of codec addresses that answered. It is
     * write-1-to-clear and deliberately left uncleared here: nothing in this
     * driver uses unsolicited responses or WAKEEN, and leaving it set makes
     * "a codec answered" observable to hda_dump() and to the KUnit suite
     * long after init. */
    uint16_t sts = mmio_r16(HDA_REG_STATESTS);
    for (uint8_t i = 0; i < 15; i++) {
        if (sts & (1u << i)) {
            g_codec = i;
            return HDA_OK;
        }
    }
    g_codec = 0xFF;
    return HDA_ENOCODEC;
}

/* ── CORB / RIRB ───────────────────────────────────────────────────────── */

static int rings_init(void)
{
    if (g_corb == NULL) {
        /* Two contiguous pages: CORB (256 x 4 B) in the first, RIRB
         * (256 x 8 B) in the second. Both bases must be 128-byte aligned;
         * page alignment is stricter than that, so it comes for free. */
        void *pages = alloc_pages_contig_owned(PAGE_OWNER_KERNEL, 2);
        if (pages == NULL) {
            return HDA_ENOMEM;
        }
        g_corb = (uint32_t *)pages;
        g_rirb = (uint32_t *)((uint8_t *)pages + 4096);
    }

    for (uint32_t i = 0; i < HDA_RING_ENTRIES; i++) {
        g_corb[i] = 0;
        g_rirb[i * 2] = 0;
        g_rirb[i * 2 + 1] = 0;
    }

    uint64_t corb_phys = (uint64_t)(uintptr_t)g_corb;
    uint64_t rirb_phys = (uint64_t)(uintptr_t)g_rirb;

    /* CORB. Stop the engine before repointing it; the size register is
     * read-only while it runs. */
    mmio_w8(HDA_REG_CORBCTL, 0);
    (void)poll_reg(1, HDA_REG_CORBCTL, HDA_CORBCTL_RUN, 0);
    mmio_w8(HDA_REG_CORBSIZE, HDA_RINGSIZE_256);
    mmio_w32(HDA_REG_CORBLBASE, (uint32_t)corb_phys);
    mmio_w32(HDA_REG_CORBUBASE, (uint32_t)(corb_phys >> 32));

    /* The read-pointer reset is a two-step handshake: set bit 15, wait for
     * the controller to acknowledge by reading it back set, then clear it
     * and wait for that. Neither wait is fatal -- Linux's azx_reset_corb_rp()
     * warns and continues too, because some controllers (QEMU's included)
     * complete the reset synchronously and never present the intermediate
     * state to be observed. What matters is the final value, asserted below. */
    mmio_w16(HDA_REG_CORBRP, HDA_CORBRP_RST);
    (void)poll_reg(2, HDA_REG_CORBRP, HDA_CORBRP_RST, HDA_CORBRP_RST);
    mmio_w16(HDA_REG_CORBRP, 0);
    (void)poll_reg(2, HDA_REG_CORBRP, HDA_CORBRP_RST, 0);

    mmio_w16(HDA_REG_CORBWP, 0);
    g_corb_wp = 0;
    mmio_w8(HDA_REG_CORBCTL, HDA_CORBCTL_RUN);

    /* RIRB. RINTCNT = 1 means "count every response"; the response
     * interrupt itself stays disabled (RIRBCTL bit 0 clear) because
     * hda_codec_verb() polls -- see its comment. */
    mmio_w8(HDA_REG_RIRBCTL, 0);
    mmio_w8(HDA_REG_RIRBSIZE, HDA_RINGSIZE_256);
    mmio_w32(HDA_REG_RIRBLBASE, (uint32_t)rirb_phys);
    mmio_w32(HDA_REG_RIRBUBASE, (uint32_t)(rirb_phys >> 32));
    mmio_w16(HDA_REG_RIRBWP, HDA_RIRBWP_RST);
    /*
     * RINTCNT is "raise the response interrupt every N responses", but it is
     * also a *flow-control* limit on the command engine: a controller stops
     * fetching from the CORB once N unread responses have accumulated and
     * only resumes when software clears RIRBSTS.RINTFL. QEMU implements
     * exactly that (hw/audio/intel-hda.c's intel_hda_corb_run() bails out on
     * `rirb_count == rirb_cnt`), so the obvious RINTCNT = 1 stalls the ring
     * after a single verb unless every caller clears RIRBSTS first --
     * which is why hda_codec_verb() clears it on both sides of a command.
     * 0xFF keeps the throttle far away from a driver that sends verbs one at
     * a time; the interrupt it counts toward is disabled anyway (RINTCTL
     * stays clear, see below).
     */
    mmio_w16(HDA_REG_RINTCNT, 0xFF);
    g_rirb_rp = 0;
    mmio_w8(HDA_REG_RIRBSTS, 0x05);      /* clear RINTFL | RIRBOIS          */
    mmio_w8(HDA_REG_RIRBCTL, HDA_RIRBCTL_DMAEN);

    if ((mmio_r8(HDA_REG_CORBCTL) & HDA_CORBCTL_RUN) == 0 ||
        (mmio_r8(HDA_REG_RIRBCTL) & HDA_RIRBCTL_DMAEN) == 0) {
        return HDA_ETIMEDOUT;
    }
    return HDA_OK;
}

int hda_codec_verb(uint8_t codec, uint8_t node, uint32_t verb, uint32_t *resp)
{
    if (!g_present || g_corb == NULL) {
        return HDA_ENODEV;
    }

    uint32_t cmd = ((uint32_t)(codec & 0xF) << 28)
                 | ((uint32_t)node << 20)
                 | (verb & 0xFFFFF);

    uint64_t flags = irq_save();

    /* Clear any latched response-interrupt flag before sending: it is what
     * releases the controller's CORB fetch throttle (see RINTCNT in
     * rings_init()). Safe unconditionally -- the previous response has
     * already been consumed by the time another verb is sent. */
    mmio_w8(HDA_REG_RIRBSTS, 0x05);

    uint16_t wp = (uint16_t)((g_corb_wp + 1) % HDA_RING_ENTRIES);
    g_corb[wp] = cmd;
    /* The controller reads the ring by DMA, so the entry has to be in
     * memory before the write pointer that publishes it. There is no cache
     * flush to do on x86 (DMA snoops), but the compiler must not reorder. */
    __asm__ volatile ("" ::: "memory");
    mmio_w16(HDA_REG_CORBWP, wp);
    g_corb_wp = wp;

    uint32_t i;
    for (i = 0; i < HDA_POLL_TRIES; i++) {
        if ((mmio_r16(HDA_REG_RIRBWP) & 0xFF) != g_rirb_rp) {
            break;
        }
        io_wait();
    }
    if (i == HDA_POLL_TRIES) {
        irq_restore(flags);
        return HDA_ETIMEDOUT;
    }

    g_rirb_rp = (uint16_t)((g_rirb_rp + 1) % HDA_RING_ENTRIES);
    if (resp != NULL) {
        *resp = g_rirb[g_rirb_rp * 2];
    }
    mmio_w8(HDA_REG_RIRBSTS, 0x05);

    irq_restore(flags);
    return HDA_OK;
}

/* ── Codec widget walk ─────────────────────────────────────────────────── */

static int verb_get(uint8_t node, uint32_t param, uint32_t *out)
{
    return hda_codec_verb(g_codec, node,
                          HDA_VERB_GET_PARAMETER | (param & 0xFF), out);
}

static int codec_setup(void)
{
    uint32_t r;

    /* Node 0's subordinate-node list names the function groups. */
    if (verb_get(0, HDA_PARAM_NODE_COUNT, &r) != HDA_OK) {
        return HDA_ETIMEDOUT;
    }
    int fg_start = (int)((r >> 16) & 0xFF);
    int fg_count = (int)(r & 0xFF);

    g_afg = 0;
    for (int n = fg_start; n < fg_start + fg_count && n < 256; n++) {
        if (verb_get((uint8_t)n, HDA_PARAM_FUNC_TYPE, &r) != HDA_OK) {
            continue;
        }
        if ((r & 0xFF) == HDA_FUNC_TYPE_AUDIO) {
            g_afg = (uint8_t)n;
            break;
        }
    }
    if (g_afg == 0) {
        return HDA_ENOOUT;
    }

    /* The AFG's subordinate nodes are the widgets. */
    if (verb_get(g_afg, HDA_PARAM_NODE_COUNT, &r) != HDA_OK) {
        return HDA_ETIMEDOUT;
    }
    int w_start = (int)((r >> 16) & 0xFF);
    int w_count = (int)(r & 0xFF);

    g_dac = 0;
    g_pin = 0;
    uint32_t pin_caps = 0;
    for (int n = w_start; n < w_start + w_count && n < 256; n++) {
        uint32_t caps;
        if (verb_get((uint8_t)n, HDA_PARAM_AUDIO_WIDGET_CAP, &caps) != HDA_OK) {
            continue;
        }
        uint32_t type = HDA_WIDGET_TYPE(caps);
        if (type == HDA_WIDGET_AUDIO_OUT && g_dac == 0) {
            g_dac = (uint8_t)n;
        } else if (type == HDA_WIDGET_PIN_COMPLEX && g_pin == 0) {
            uint32_t pcap;
            if (verb_get((uint8_t)n, HDA_PARAM_PIN_CAP, &pcap) == HDA_OK &&
                (pcap & HDA_PINCAP_OUTPUT)) {
                g_pin = (uint8_t)n;
                pin_caps = caps;
            }
        }
    }
    if (g_dac == 0) {
        return HDA_ENOOUT;
    }

    /* Power the function group and the widgets up before configuring them:
     * a codec parked in D3 accepts the verbs and produces no sound. */
    hda_codec_verb(g_codec, g_afg, HDA_VERB_SET_POWER_STATE | 0x00, NULL);
    hda_codec_verb(g_codec, g_dac, HDA_VERB_SET_POWER_STATE | 0x00, NULL);
    if (g_pin) {
        hda_codec_verb(g_codec, g_pin, HDA_VERB_SET_POWER_STATE | 0x00, NULL);
    }

    /* Tell the converter what the controller will feed it, and which stream
     * tag to listen on. Both halves are required: a DAC with a format but
     * no stream tag ignores the link entirely. */
    hda_codec_verb(g_codec, g_dac,
                   HDA_VERB_SET_CONVERTER_FORMAT | HDA_FMT_48K_16BIT_STEREO,
                   NULL);
    hda_codec_verb(g_codec, g_dac,
                   HDA_VERB_SET_STREAM_CHANNEL | (HDA_STREAM_TAG << 4) | 0,
                   NULL);

    /*
     * Unmute. The amp payload is: bit 15 "set output amp", bit 13/12
     * "left/right", bit 7 mute, bits 6:0 gain. The gain is the widget's own
     * maximum, read from OUT_AMP_CAP, because a hardcoded value is either
     * inaudible or clipped depending on the codec -- and it is the *step
     * count* field that says what the maximum is (see HDA_AMPCAP_NUM_STEPS;
     * taking the step-size field next to it instead sets a gain of 3 out of
     * 74 on QEMU, which plays ~28 dB down and sounds like silence). A widget
     * that advertises no output amp is skipped rather than sent a verb it
     * will ignore.
     */
    for (int which = 0; which < 2; which++) {
        uint8_t node = (which == 0) ? g_dac : g_pin;
        if (node == 0) {
            continue;
        }
        uint32_t cap;
        if (verb_get(node, HDA_PARAM_OUT_AMP_CAP, &cap) != HDA_OK || cap == 0) {
            continue;
        }
        uint32_t gain = HDA_AMPCAP_NUM_STEPS(cap);
        hda_codec_verb(g_codec, node,
                       HDA_VERB_SET_AMP_GAIN_MUTE | 0xB000u | gain, NULL);
    }

    if (g_pin != 0) {
        hda_codec_verb(g_codec, g_pin,
                       HDA_VERB_SET_PIN_WIDGET_CONTROL | HDA_PIN_CTL_OUT_ENABLE,
                       NULL);
        /* External amplifier power-down is active-low on the pins that have
         * it; without this a laptop's speakers stay silent while every
         * register reads correct. */
        if (pin_caps & HDA_WIDGET_CAP_EAPD) {
            hda_codec_verb(g_codec, g_pin, HDA_VERB_SET_EAPD_BTL | 0x02, NULL);
        }
    }

    return HDA_OK;
}

/* ── Sample buffer, BDL and stream descriptor ──────────────────────────── */

/*
 * Fill the sample buffer with a square wave and return the frequency
 * actually produced. The buffer loops, so the wave has to close on itself:
 * `periods` is rounded to a whole number and the phase is computed from it
 * exactly, which snaps the frequency to a multiple of
 * 48000 / 16384 = ~2.93 Hz. Anything else leaves a partial period at the
 * wrap point, which is an audible click 2.9 times a second.
 *
 * Integer throughout -- every kernel C file compiles -mno-sse and so cannot
 * use a double
 * (see CLAUDE.md); a square wave needs no trigonometry anyway.
 */
static uint32_t fill_tone(uint32_t freq_hz)
{
    uint32_t periods = (uint32_t)(((uint64_t)freq_hz * HDA_BUF_FRAMES
                                   + HDA_SAMPLE_RATE_HZ / 2)
                                  / HDA_SAMPLE_RATE_HZ);
    if (periods == 0) {
        periods = 1;
    }

    for (uint32_t i = 0; i < HDA_BUF_FRAMES; i++) {
        uint32_t phase = (uint32_t)(((uint64_t)i * periods) % HDA_BUF_FRAMES);
        int16_t s = (phase * 2u < HDA_BUF_FRAMES)
                    ? (int16_t)HDA_TONE_AMPLITUDE
                    : (int16_t)(-HDA_TONE_AMPLITUDE);
        g_buf[i * 2]     = s;
        g_buf[i * 2 + 1] = s;
    }

    return (uint32_t)(((uint64_t)periods * HDA_SAMPLE_RATE_HZ) / HDA_BUF_FRAMES);
}

/*
 * Dice the sample buffer into `entries` equal BDL entries, each asking for a
 * completion interrupt.
 *
 * Never one entry: the spec requires LVI >= 1, so a single-entry list is
 * illegal. Two is what a tone wants -- it also halves the latency between
 * "DMA is running" and the first observable interrupt. Streaming wants
 * HDA_BDL_ENTRIES_PCM, for the latency reason that constant's own comment
 * gives. The buffer and its physical pages are identical either way; only the
 * dicing differs, so this rewrites the list in place and records the count
 * for stream_program() to derive LVI from.
 */
static void bdl_program(uint32_t entries)
{
    uint64_t buf_phys = (uint64_t)(uintptr_t)g_buf;
    uint32_t len = HDA_BUF_BYTES / entries;

    for (uint32_t i = 0; i < entries; i++) {
        g_bdl[i].addr  = buf_phys + (uint64_t)i * len;
        g_bdl[i].len   = len;
        g_bdl[i].flags = HDA_BDL_IOC;
    }
    g_bdl_entries = entries;
}

static int buffers_init(void)
{
    if (g_bdl == NULL) {
        void *p = alloc_pages_contig_owned(PAGE_OWNER_KERNEL, 1);
        if (p == NULL) {
            return HDA_ENOMEM;
        }
        g_bdl = (hda_bdl_entry_t *)p;
    }
    if (g_buf == NULL) {
        void *p = alloc_pages_contig_owned(PAGE_OWNER_KERNEL, HDA_BUF_PAGES);
        if (p == NULL) {
            return HDA_ENOMEM;
        }
        g_buf = (int16_t *)p;
    }

    bdl_program(HDA_BDL_ENTRIES);
    return HDA_OK;
}

/* Reset and reprogram the output stream descriptor. Leaves RUN clear. */
static int stream_program(void)
{
    /* Stop first; SRST is only meaningful on a stopped stream. */
    mmio_w32(sd(HDA_SD_CTL), 0);
    (void)poll_reg(4, sd(HDA_SD_CTL), HDA_SDCTL_RUN, 0);

    /*
     * The stream reset handshake, same shape as the CORB one above and with
     * the same caveat: QEMU completes the reset inside the register write
     * and never shows SRST set, so waiting for it to read back as 1 times
     * out on every boot. Linux tolerates that too (azx_stream_reset()'s
     * bounded loop simply falls through). The wait for it to read back as 0
     * is the one that matters, and it is checked.
     */
    mmio_w32(sd(HDA_SD_CTL), HDA_SDCTL_SRST);
    delay_us(10);
    (void)poll_reg(4, sd(HDA_SD_CTL), HDA_SDCTL_SRST, HDA_SDCTL_SRST);
    mmio_w32(sd(HDA_SD_CTL), 0);
    delay_us(10);
    if (poll_reg(4, sd(HDA_SD_CTL), HDA_SDCTL_SRST, 0) != HDA_OK) {
        return HDA_ETIMEDOUT;
    }

    /* Clear any status the previous run latched, before the new one starts
     * accumulating: BCIS left set from an earlier tone holds INTx asserted,
     * and a stale FIFOE/DESE would be misread as this stream's error. */
    mmio_w8(sd(HDA_SD_STS),
            HDA_SDSTS_BCIS | HDA_SDSTS_FIFOE | HDA_SDSTS_DESE);

    mmio_w32(sd(HDA_SD_CBL), HDA_BUF_BYTES);
    mmio_w16(sd(HDA_SD_LVI), (uint16_t)(g_bdl_entries - 1));
    mmio_w16(sd(HDA_SD_FMT), HDA_FMT_48K_16BIT_STEREO);

    uint64_t bdl_phys = (uint64_t)(uintptr_t)g_bdl;
    mmio_w32(sd(HDA_SD_BDPL), (uint32_t)bdl_phys);
    mmio_w32(sd(HDA_SD_BDPU), (uint32_t)(bdl_phys >> 32));

    /* Stream tag and the per-buffer completion interrupt. The top byte of
     * this dword is SDnSTS; writing zeros there is a no-op on a W1C field,
     * so this does not disturb any latched status. */
    mmio_w32(sd(HDA_SD_CTL),
             ((uint32_t)HDA_STREAM_TAG << HDA_SDCTL_TAG_SHIFT)
             | HDA_SDCTL_IOCE);

    /* Route this descriptor's interrupt to the pin. GIE is the master
     * enable; bit n is stream descriptor n. */
    mmio_w32(HDA_REG_INTCTL, HDA_INTCTL_GIE | (1u << g_stream));
    return HDA_OK;
}

/* ── Interrupt wiring ──────────────────────────────────────────────────── */

static int irq_init(void)
{
    uint8_t pin  = pci_config_read8(g_dev->bus, g_dev->device, g_dev->function,
                                    PCI_OFF_INTERRUPT_PIN);
    uint8_t line = pci_config_read8(g_dev->bus, g_dev->device, g_dev->function,
                                    PCI_OFF_INTERRUPT_LINE);

    if (pin == 0 || line == 0xFF || line > 15) {
        return HDA_ENOIRQ;
    }
    /*
     * INTx is shareable, and this kernel has no shared-IRQ dispatch: one
     * vector, one handler. IRQ0 (PIT), IRQ1 (keyboard) and IRQ2 (the slave
     * cascade) already have owners, so a controller routed onto one of them
     * would have this driver's stub replace theirs. Refuse instead and stay
     * polled -- a silent tone is recoverable, a dead timer is not. QEMU
     * routes HDA to IRQ 11 in practice; the line is read, never assumed.
     */
    if (line <= 2) {
        return HDA_ENOIRQ;
    }

    /* Vector first, unmask second -- the same ordering kernel_main uses for
     * IRQ1/kbd_init(). The other way round, an interrupt arriving in between
     * lands on idt_init()'s default_stub, whose bare iretq sends no EOI and
     * wedges the line for the rest of the boot (see src/pic.c). */
    idt_set_gate(32 + (int)line, (uintptr_t)irq_hda_stub);
    g_irq_line = line;
    if (line >= 8) {
        pic_unmask_irq(2);
    }
    pic_unmask_irq(line);
    return HDA_OK;
}

/*
 * Render into every BDL entry the DMA engine has finished with, up to but not
 * including the one it is reading now.
 *
 * SDnLPIB is the stream's byte position inside the cyclic buffer, so
 * LPIB / HDA_PCM_ENTRY_BYTES is the entry currently being read. Everything
 * from g_pcm_write up to there has been consumed and is free to overwrite;
 * the entry being read is not, and neither is anything ahead of it. Writing
 * the just-consumed entries puts the fill as far from the read head as the
 * buffer allows -- almost a full loop of headroom -- which is what makes a
 * 21 ms slice safe rather than marginal.
 *
 * Catching up in a loop, rather than assuming exactly one entry per
 * interrupt, is what makes a coalesced or missed completion cost one longer
 * render instead of a permanent hole in the output. The loop is bounded by
 * the entry count, so the worst case is the whole buffer once.
 *
 * Runs inside hda_irq_handler() with IF clear, so pcm_mixer_render() sees a
 * stable voice table without taking a lock of its own.
 */
static void pcm_refill(void)
{
    uint32_t lpib    = mmio_r32(sd(HDA_SD_LPIB));
    uint32_t playing = (lpib / HDA_PCM_ENTRY_BYTES) % HDA_BDL_ENTRIES_PCM;

    for (uint32_t guard = 0; guard < HDA_BDL_ENTRIES_PCM; guard++) {
        if (g_pcm_write == playing) {
            break;
        }
        int16_t *slice = g_buf + (size_t)g_pcm_write
                                 * (HDA_PCM_ENTRY_BYTES / sizeof(int16_t));
        pcm_mixer_render(slice, HDA_PCM_ENTRY_FRAMES);
        g_pcm_write = (g_pcm_write + 1) % HDA_BDL_ENTRIES_PCM;
        g_pcm_refills++;
    }
}

void hda_irq_handler(void)
{
    if (g_bar != NULL) {
        uint32_t ints = mmio_r32(HDA_REG_INTSTS);
        if (ints & (1u << g_stream)) {
            uint8_t sts = mmio_r8(sd(HDA_SD_STS));

            g_stream_err |= (uint8_t)(sts & (HDA_SDSTS_FIFOE | HDA_SDSTS_DESE));

            /* Clear what was set, through the byte register -- the only
             * access that can (see HDA_SD_STS's comment in hda.h). This is
             * what deasserts the controller's level-triggered INTx line; a
             * clear that misses means this handler is re-entered forever. */
            mmio_w8(sd(HDA_SD_STS),
                    (uint8_t)(sts & (HDA_SDSTS_BCIS | HDA_SDSTS_FIFOE
                                     | HDA_SDSTS_DESE)));
            g_irq_count++;

            /*
             * Streaming mode's refill (SCRUM-212). Deliberately *after* the
             * status clear: that write is what deasserts the level-triggered
             * INTx line, and the mixing below is by far the longest thing
             * this handler does. Clearing first keeps the line down for the
             * whole render rather than holding it asserted throughout.
             */
            if (g_streaming) {
                if (sts & HDA_SDSTS_FIFOE) {
                    /* The controller reached a slice the mixer had not
                     * written in time. Counted rather than ignored: it is the
                     * only failure a too-slow render can produce, and
                     * hda_pcm_underruns() is what asserts it does not happen.
                     */
                    g_pcm_underruns++;
                }
                pcm_refill();
            }
        }
        /* A call with our bit clear is somebody else's shared interrupt.
         * Nothing to do -- but the EOI below still has to happen, because
         * the PIC set an in-service bit either way. */
    }

    if (g_irq_line != 0xFF) {
        pic_send_EOI(g_irq_line);
    }
}

/* ── Public API ────────────────────────────────────────────────────────── */

int hda_init(void)
{
    g_present = 0;

    int rc = controller_find();
    if (rc != HDA_OK) {
        return rc;
    }

    uint16_t gcap = mmio_r16(HDA_REG_GCAP);
    if (gcap == 0xFFFF || HDA_GCAP_OSS(gcap) == 0) {
        /* All-ones means the BAR window is not decoding; no output stream
         * means there is nothing this driver could ever do with it. */
        return HDA_ENODEV;
    }
    /* Output descriptors follow the input ones in the register block. */
    g_stream = (uint8_t)HDA_GCAP_ISS(gcap);

    rc = controller_reset();
    if (rc != HDA_OK) {
        return rc;
    }

    rc = codec_detect();
    if (rc != HDA_OK) {
        return rc;
    }

    rc = rings_init();
    if (rc != HDA_OK) {
        return rc;
    }

    /* hda_codec_verb() refuses to run until the driver calls itself
     * present, and codec_setup() is nothing but verbs. */
    g_present = 1;

    rc = codec_setup();
    if (rc != HDA_OK) {
        g_present = 0;
        return rc;
    }

    rc = buffers_init();
    if (rc != HDA_OK) {
        g_present = 0;
        return rc;
    }

    rc = stream_program();
    if (rc != HDA_OK) {
        g_present = 0;
        return rc;
    }

    rc = irq_init();
    if (rc != HDA_OK) {
        /* Everything except completion interrupts still works; a caller can
         * poll hda_stream_position(). Report it and keep the controller. */
        serial_print("hda: no usable INTx line, completion IRQ disabled\n");
    }

    return HDA_OK;
}

int hda_present(void)
{
    return g_present;
}

int hda_play_tone(uint32_t freq_hz, uint32_t dur_ms)
{
    if (!g_present || g_buf == NULL) {
        return HDA_ENODEV;
    }
    if (freq_hz < HDA_MIN_HZ || freq_hz > HDA_MAX_HZ) {
        return HDA_EINVAL;
    }

    /* Fill before the stream is touched: the DMA engine is stopped by
     * stream_program()'s reset, so there is no window where the controller
     * reads a half-written buffer. */
    uint32_t actual = fill_tone(freq_hz);

    uint64_t flags = irq_save();

    g_playing = 0;
    g_stream_err = 0;
    /* Take the stream back from streaming mode, if it had it, and restore the
     * tone's own BDL split. Each mode programs its own layout on entry, so
     * neither has to undo the other's on exit (SCRUM-212). */
    g_streaming = 0;
    bdl_program(HDA_BDL_ENTRIES);
    int rc = stream_program();
    if (rc != HDA_OK) {
        irq_restore(flags);
        return rc;
    }

    mmio_w32(sd(HDA_SD_CTL),
             ((uint32_t)HDA_STREAM_TAG << HDA_SDCTL_TAG_SHIFT)
             | HDA_SDCTL_IOCE | HDA_SDCTL_RUN);

    g_tone_hz    = actual;
    g_timed      = (dur_ms != 0);
    g_deadline_ms = kernel_get_ticks_ms() + dur_ms;
    g_playing    = 1;

    irq_restore(flags);
    return HDA_OK;
}

void hda_stop(void)
{
    if (!g_present) {
        return;
    }
    uint64_t flags = irq_save();
    mmio_w32(sd(HDA_SD_CTL), 0);
    g_playing = 0;
    g_timed = 0;
    g_streaming = 0;
    irq_restore(flags);
}

int hda_is_playing(void)
{
    return g_playing;
}

uint32_t hda_tone_hz(void)
{
    return g_tone_hz;
}

/* ── Streaming (PCM) mode ──────────────────────────────────────────────── */

int hda_pcm_start(void)
{
    if (!g_present || g_buf == NULL) {
        return HDA_ENODEV;
    }

    /*
     * Prefill the whole buffer before the DMA engine is pointed at it. The
     * stream is stopped by stream_program()'s reset, so there is no window in
     * which the controller reads a half-written slice -- and because
     * pcm_mixer_render() writes every frame it is given, this also guarantees
     * the buffer holds no leftovers from a previous tone. With no voices
     * started yet that fill is silence, which is exactly what should be
     * playing until something starts.
     */
    pcm_mixer_render(g_buf, HDA_BUF_FRAMES);

    uint64_t flags = irq_save();

    g_playing       = 0;
    g_streaming     = 0;   /* not yet: pcm_refill() must not run mid-setup */
    g_stream_err    = 0;
    g_pcm_refills   = 0;
    g_pcm_underruns = 0;

    bdl_program(HDA_BDL_ENTRIES_PCM);
    int rc = stream_program();
    if (rc != HDA_OK) {
        irq_restore(flags);
        return rc;
    }

    /*
     * The refill cursor starts at entry 0 while the DMA engine starts there
     * too, so the first completion interrupt finds them one apart and renders
     * exactly the entry just consumed. Every entry already holds valid
     * samples from the prefill above, so nothing is played before it is
     * written even on the first loop.
     */
    g_pcm_write = 0;

    mmio_w32(sd(HDA_SD_CTL),
             ((uint32_t)HDA_STREAM_TAG << HDA_SDCTL_TAG_SHIFT)
             | HDA_SDCTL_IOCE | HDA_SDCTL_RUN);

    /* No deadline: a mixed stream ends when its voices do, not on a timer, so
     * hda_tick() has nothing to do here (it only acts on g_timed). */
    g_timed     = 0;
    g_tone_hz   = 0;
    g_playing   = 1;
    g_streaming = 1;

    irq_restore(flags);
    return HDA_OK;
}

void hda_pcm_stop(void)
{
    if (!g_present) {
        return;
    }
    uint64_t flags = irq_save();
    mmio_w32(sd(HDA_SD_CTL), 0);
    g_streaming = 0;
    g_playing   = 0;
    g_timed     = 0;
    irq_restore(flags);
}

int hda_pcm_is_streaming(void)
{
    return g_streaming;
}

uint32_t hda_pcm_refills(void)
{
    return g_pcm_refills;
}

uint32_t hda_pcm_underruns(void)
{
    return g_pcm_underruns;
}

void hda_tick(uint32_t now_ms)
{
    /* Runs inside irq0_handler() with IF clear, so nothing interleaves. */
    if (g_playing && g_timed && (int32_t)(now_ms - g_deadline_ms) >= 0) {
        mmio_w32(sd(HDA_SD_CTL), 0);
        g_playing = 0;
        g_timed = 0;
    }
}

/* ── Introspection ─────────────────────────────────────────────────────── */

uint32_t hda_reg_read32(uint32_t off) { return g_bar ? mmio_r32(off) : 0; }
uint16_t hda_reg_read16(uint32_t off) { return g_bar ? mmio_r16(off) : 0; }
uint8_t  hda_reg_read8 (uint32_t off) { return g_bar ? mmio_r8(off)  : 0; }

uint32_t hda_sd_read32(uint32_t off) { return g_bar ? mmio_r32(sd(off)) : 0; }
uint16_t hda_sd_read16(uint32_t off) { return g_bar ? mmio_r16(sd(off)) : 0; }

uint8_t hda_sd_status(void)
{
    return g_bar ? mmio_r8(sd(HDA_SD_STS)) : 0;
}

uint64_t hda_bar_base(void)   { return g_bar_base; }
uint8_t  hda_codec_addr(void) { return g_codec; }
uint8_t  hda_dac_node(void)   { return g_dac; }
uint8_t  hda_pin_node(void)   { return g_pin; }
uint8_t  hda_stream_index(void) { return g_stream; }
uint8_t  hda_irq_line(void)   { return g_irq_line; }

uint64_t hda_corb_phys(void) { return (uint64_t)(uintptr_t)g_corb; }
uint64_t hda_rirb_phys(void) { return (uint64_t)(uintptr_t)g_rirb; }
uint64_t hda_bdl_phys(void)  { return (uint64_t)(uintptr_t)g_bdl; }
uint64_t hda_buf_phys(void)  { return (uint64_t)(uintptr_t)g_buf; }

uint32_t hda_stream_position(void)
{
    return g_bar ? mmio_r32(sd(HDA_SD_LPIB)) : 0;
}

uint32_t hda_irq_count(void)    { return g_irq_count; }
uint8_t  hda_stream_errors(void) { return g_stream_err; }

void hda_dump(void)
{
    if (!g_present) {
        serial_print("hda: not present\n");
        return;
    }

    uint16_t gcap = mmio_r16(HDA_REG_GCAP);
    serial_print("hda: bar=0x");
    serial_print_hex64(g_bar_base);
    serial_print(" version ");
    serial_print_dec(mmio_r8(HDA_REG_VMAJ));
    serial_print(".");
    serial_print_dec(mmio_r8(HDA_REG_VMIN));
    serial_print(" oss=");
    serial_print_dec(HDA_GCAP_OSS(gcap));
    serial_print(" iss=");
    serial_print_dec(HDA_GCAP_ISS(gcap));
    serial_print("\n");

    serial_print("hda: codec=");
    serial_print_dec(g_codec);
    serial_print(" afg=");
    serial_print_dec(g_afg);
    serial_print(" dac=");
    serial_print_dec(g_dac);
    serial_print(" pin=");
    serial_print_dec(g_pin);
    serial_print(" statests=0x");
    serial_print_hex(mmio_r16(HDA_REG_STATESTS));
    serial_print("\n");

    serial_print("hda: stream=");
    serial_print_dec(g_stream);
    serial_print(" irq=");
    if (g_irq_line == 0xFF) {
        serial_print("none");
    } else {
        serial_print_dec(g_irq_line);
    }
    serial_print(" corb=0x");
    serial_print_hex64((uint64_t)(uintptr_t)g_corb);
    serial_print(" rirb=0x");
    serial_print_hex64((uint64_t)(uintptr_t)g_rirb);
    serial_print("\n");

    serial_print("hda: bdl=0x");
    serial_print_hex64((uint64_t)(uintptr_t)g_bdl);
    serial_print(" buf=0x");
    serial_print_hex64((uint64_t)(uintptr_t)g_buf);
    serial_print(" cbl=");
    serial_print_dec(mmio_r32(sd(HDA_SD_CBL)));
    serial_print("\n");
}
