/*
 * test_hda_k.c — Intel HDA controller, CORB/RIRB and stream DMA (SCRUM-210).
 *
 * Drives the real src/hda.c against whatever QEMU emulates, not a mock --
 * the same reasoning tests/kernel/test_pci_k.c states for the bus underneath
 * it: there is no way to fake an HDA controller's MMIO register block or a
 * codec's verb responses from inside the kernel, and a mock would prove
 * nothing about the one thing this ticket is for.
 *
 * The acceptance case is unconditional. docker-test/docker-ci pass
 * `-device intel-hda -device hda-output,audiodev=snd0 -audiodev none,id=snd0`
 * (see the Makefile), so a build that stops finding the controller, the
 * codec, or a DAC is a regression in this driver, not a machine without a
 * sound card. hda_init() itself has already run from kernel_main by the time
 * any of this executes.
 *
 * ── What "proven audibly" means here ──────────────────────────────────────
 *
 * The ticket asks for a tone that plays end to end, proven the way SCRUM-98's
 * speaker driver was: hardware state asserted from a kernel test rather than
 * "sounds right". Headless Docker has no audio backend, so what is asserted
 * below is the whole chain up to the point where samples leave RAM --
 * SDnLPIB advancing proves the controller is really reading the buffer by
 * DMA, and a completion interrupt arriving proves it finished a BDL entry and
 * raised INTx through the PIC. Audibility itself is `make run`'s job, with a
 * real -audiodev on a machine that has a speaker.
 *
 * ── Interrupts ────────────────────────────────────────────────────────────
 *
 * kernel_main does no blanket `sti` before run_tests() (see
 * test_doomgeneric_timer_k.c's header for why no suite may assume ambient IF
 * in either direction), so the two tests that need time to pass enable
 * interrupts for exactly their own wait and clear them again afterwards.
 * They have to: QEMU advances a stream's DMA position from a timer callback,
 * and the completion interrupt is the thing under test.
 */

#include "kunit.h"
#include "hda.h"
#include "pci.h"
#include "pit.h"

#include <stdint.h>

/*
 * How long to let the stream run. One BDL entry is half of HDA_BUF_BYTES =
 * 32768 bytes = 8192 frames = ~171 ms at 48 kHz, so a completion interrupt
 * cannot arrive sooner than that. 400 ms leaves better than a 2x margin and
 * is still nothing against the test boot's 30-second budget.
 */
#define HDA_TEST_WAIT_MS 400u

/* Frequency used throughout. Snapped by hda_play_tone() to a multiple of
 * 48000/16384 (~2.93 Hz), so assertions compare against hda_tone_hz(). */
#define HDA_TEST_HZ 440u

/* Spin with interrupts enabled until `ms` have passed. `hlt` rather than a
 * busy loop so IRQ0 (and the HDA line) actually get serviced promptly. */
static void wait_ms_with_irqs(uint32_t ms)
{
    uint32_t start = kernel_get_ticks_ms();
    __asm__ volatile ("sti");
    while ((uint32_t)(kernel_get_ticks_ms() - start) < ms) {
        __asm__ volatile ("hlt");
    }
    __asm__ volatile ("cli");
}

/* Leave the controller quiet for whatever suite runs next: a stream left
 * running would keep raising IRQ 11 into a vector this suite no longer cares
 * about. */
static int suite_cleanup(void)
{
    hda_stop();
    return 0;
}

static void test_controller_found_and_mapped(void)
{
    /* hda_init() ran from kernel_main; if it did not complete, everything
     * below is meaningless, so this is the assertion that localises a
     * failure to "the controller was never brought up". */
    CU_ASSERT_TRUE(hda_present());

    /* The BAR has to be a real programmed window. All-ones GCAP would mean
     * the memory decoder is off or the mapping missed. */
    CU_ASSERT_NOT_EQUAL(hda_bar_base(), 0);
    uint16_t gcap = hda_reg_read16(HDA_REG_GCAP);
    CU_ASSERT_NOT_EQUAL(gcap, 0xFFFF);

    /* At least one output stream descriptor, or there is nothing to play
     * through. QEMU's ICH6 reports 4 in and 4 out. */
    CU_ASSERT(HDA_GCAP_OSS(gcap) >= 1);

    /* The output descriptors follow the input ones, so the index the driver
     * picked must be exactly GCAP.ISS -- hardcoding 0 here (or in the
     * driver) would address an *input* stream on any real controller. */
    CU_ASSERT_EQUAL(hda_stream_index(), HDA_GCAP_ISS(gcap));

    /* Bus mastering is what lets the controller DMA the BDL at all; without
     * it every register below still reads correct and nothing ever plays. */
    const pci_device_t *dev = pci_find_class(PCI_CLASS_MULTIMEDIA,
                                            PCI_SUBCLASS_AUDIO_DEV);
    CU_ASSERT_PTR_NOT_NULL(dev);
    if (dev != NULL) {
        uint16_t cmd = pci_config_read16(dev->bus, dev->device, dev->function,
                                         PCI_OFF_COMMAND);
        CU_ASSERT(cmd & PCI_CMD_BUS_MASTER);
        CU_ASSERT(cmd & PCI_CMD_MEM_SPACE);
    }
}

static void test_reset_completed_and_codec_answered(void)
{
    /* CRST reads back 1 only once the controller has left reset. A 0 here is
     * the state a missing or too-short reset wait leaves behind, and every
     * verb below would time out against it. */
    CU_ASSERT(hda_reg_read32(HDA_REG_GCTL) & HDA_GCTL_CRST);

    /* STATESTS is the codec-presence bitmap, sampled after the spec's 521 us
     * enumeration window. Zero means no codec on the link -- which on this
     * QEMU command line means the `-device hda-output` flag went missing,
     * not that the driver regressed. */
    CU_ASSERT_NOT_EQUAL(hda_reg_read16(HDA_REG_STATESTS), 0);

    /* And the driver picked an address out of it rather than leaving the
     * "none found" sentinel. */
    CU_ASSERT(hda_codec_addr() < 15);
}

static void test_corb_rirb_rings_running(void)
{
    /* Both DMA engines enabled: the CORB fetches commands, the RIRB writes
     * responses. Either one stopped makes hda_codec_verb() a timeout. */
    CU_ASSERT(hda_reg_read8(HDA_REG_CORBCTL) & HDA_CORBCTL_RUN);
    CU_ASSERT(hda_reg_read8(HDA_REG_RIRBCTL) & HDA_RIRBCTL_DMAEN);

    /* The base registers must name the pages the driver actually allocated.
     * This is the check that catches a ring pointed at the wrong physical
     * address -- which on an identity-mapped kernel is otherwise invisible,
     * because reading and writing the ring from C would still work. */
    CU_ASSERT_NOT_EQUAL(hda_corb_phys(), 0);
    CU_ASSERT_NOT_EQUAL(hda_rirb_phys(), 0);
    CU_ASSERT_EQUAL(hda_reg_read32(HDA_REG_CORBLBASE),
                    (uint32_t)hda_corb_phys());
    CU_ASSERT_EQUAL(hda_reg_read32(HDA_REG_CORBUBASE),
                    (uint32_t)(hda_corb_phys() >> 32));
    CU_ASSERT_EQUAL(hda_reg_read32(HDA_REG_RIRBLBASE),
                    (uint32_t)hda_rirb_phys());
    CU_ASSERT_EQUAL(hda_reg_read32(HDA_REG_RIRBUBASE),
                    (uint32_t)(hda_rirb_phys() >> 32));

    /* The ring bases are 128-byte aligned as the spec requires (they are
     * whole pages, so this is a property of the allocator, asserted rather
     * than assumed). */
    CU_ASSERT_EQUAL(hda_corb_phys() & 0x7F, 0);
    CU_ASSERT_EQUAL(hda_rirb_phys() & 0x7F, 0);

    /* The read-pointer reset finished: bit 15 clear is the end state of the
     * two-step CORBRP handshake. */
    CU_ASSERT_EQUAL(hda_reg_read16(HDA_REG_CORBRP) & HDA_CORBRP_RST, 0);
}

static void test_verb_round_trip(void)
{
    /*
     * The single assertion that proves the command ring end to end: a verb
     * written into the CORB, fetched by the controller's DMA, answered by a
     * real codec, and written back into the RIRB by DMA where this code reads
     * it. A vendor ID of 0 or all-ones means the response slot was never
     * written -- the failure mode of a mis-programmed RIRB base or an
     * off-by-one write pointer, both of which otherwise look like success.
     */
    uint32_t vendor = 0xDEADBEEF;
    int rc = hda_codec_verb(hda_codec_addr(), 0,
                            HDA_VERB_GET_PARAMETER | HDA_PARAM_VENDOR_ID,
                            &vendor);
    CU_ASSERT_EQUAL(rc, HDA_OK);
    CU_ASSERT_NOT_EQUAL(vendor, 0u);
    CU_ASSERT_NOT_EQUAL(vendor, 0xFFFFFFFFu);
    CU_ASSERT_NOT_EQUAL(vendor, 0xDEADBEEFu);

    /* A second verb in a row, because the first one passing on its own hides
     * the ring's actual failure mode: the controller stops fetching from the
     * CORB once RINTCNT unread responses have piled up, so a driver that
     * never clears RIRBSTS works exactly once. */
    uint32_t nodes = 0;
    rc = hda_codec_verb(hda_codec_addr(), 0,
                        HDA_VERB_GET_PARAMETER | HDA_PARAM_NODE_COUNT,
                        &nodes);
    CU_ASSERT_EQUAL(rc, HDA_OK);
    CU_ASSERT_NOT_EQUAL(nodes & 0xFF, 0);       /* at least one function group */
}

static void test_codec_walk_found_an_output(void)
{
    /* Node 0 is the root and can never be a widget, so 0 is this driver's
     * "not found" value for both. */
    CU_ASSERT_NOT_EQUAL(hda_dac_node(), 0);
    CU_ASSERT_NOT_EQUAL(hda_pin_node(), 0);

    /* Confirm independently that the node the walk chose really is an output
     * converter: re-read its widget capabilities and check the type field.
     * Without this the DAC assertion above only says "some node was
     * picked". */
    uint32_t caps = 0;
    int rc = hda_codec_verb(hda_codec_addr(), hda_dac_node(),
                            HDA_VERB_GET_PARAMETER | HDA_PARAM_AUDIO_WIDGET_CAP,
                            &caps);
    CU_ASSERT_EQUAL(rc, HDA_OK);
    CU_ASSERT_EQUAL(HDA_WIDGET_TYPE(caps), HDA_WIDGET_AUDIO_OUT);

    /* And that the pin really is a pin complex. */
    rc = hda_codec_verb(hda_codec_addr(), hda_pin_node(),
                        HDA_VERB_GET_PARAMETER | HDA_PARAM_AUDIO_WIDGET_CAP,
                        &caps);
    CU_ASSERT_EQUAL(rc, HDA_OK);
    CU_ASSERT_EQUAL(HDA_WIDGET_TYPE(caps), HDA_WIDGET_PIN_COMPLEX);
}

static void test_output_amp_unmuted_at_full_gain(void)
{
    /*
     * A regression test for a bug that produced a *perfect-looking* driver:
     * every register below reads correct, DMA runs, the completion interrupt
     * fires, and the tone comes out roughly 28 dB down -- clearly there on a
     * capture, inaudible on a speaker. The cause was reading OUT_AMP_CAP's
     * step-size field (bits 22:16) where the step *count* (bits 14:8) was
     * meant, which set a gain of 3 out of QEMU's 74 steps.
     *
     * Nothing inside the kernel can measure loudness, so the check is the
     * codec's own readback: the gain it holds must be the maximum its
     * capabilities advertise, and the channel must not be muted.
     */
    uint32_t cap = 0;
    int rc = hda_codec_verb(hda_codec_addr(), hda_dac_node(),
                            HDA_VERB_GET_PARAMETER | HDA_PARAM_OUT_AMP_CAP,
                            &cap);
    CU_ASSERT_EQUAL(rc, HDA_OK);

    /* A codec whose DAC has no output amp at all is a legitimate
     * configuration -- the driver skips the verb rather than sending one that
     * would be ignored -- so there is nothing to assert in that case. QEMU's
     * hda-output codec does have one. */
    if (cap == 0) {
        return;
    }

    uint32_t steps = HDA_AMPCAP_NUM_STEPS(cap);
    CU_ASSERT_NOT_EQUAL(steps, 0);

    uint32_t amp = 0;
    rc = hda_codec_verb(hda_codec_addr(), hda_dac_node(),
                        HDA_VERB_GET_AMP_GAIN_MUTE | HDA_AMP_GET_OUTPUT_LEFT,
                        &amp);
    CU_ASSERT_EQUAL(rc, HDA_OK);

    /* Unmuted, and at the top of the widget's own range. */
    CU_ASSERT_EQUAL(amp & HDA_AMP_GET_MUTE, 0);
    CU_ASSERT_EQUAL(HDA_AMP_GET_GAIN(amp), steps);

    /* And the specific wrong answer the old code gave, called out by name so
     * a failure here says what happened rather than just "not equal". */
    CU_ASSERT_NOT_EQUAL(HDA_AMP_GET_GAIN(amp), (cap >> 16) & 0x7F);
}

static void test_stream_and_bdl_programmed(void)
{
    /* Starts the tone the next test then measures. Left running on purpose. */
    CU_ASSERT_EQUAL(hda_play_tone(HDA_TEST_HZ, 0), HDA_OK);
    CU_ASSERT_TRUE(hda_is_playing());

    uint32_t ctl = hda_sd_read32(HDA_SD_CTL);
    CU_ASSERT(ctl & HDA_SDCTL_RUN);
    CU_ASSERT(ctl & HDA_SDCTL_IOCE);

    /* The stream tag has to match what SET_CONVERTER_STREAM_CHANNEL told the
     * codec to listen for; a mismatch is silence with every register
     * otherwise correct, which is exactly the bug worth an assertion. */
    CU_ASSERT_EQUAL((ctl >> HDA_SDCTL_TAG_SHIFT) & 0xF, HDA_STREAM_TAG);

    /* Buffer geometry. CBL is the whole cyclic buffer; LVI is the *index* of
     * the last valid BDL entry, so one less than the entry count -- the
     * classic off-by-one here truncates or overruns the loop. */
    CU_ASSERT_EQUAL(hda_sd_read32(HDA_SD_CBL), HDA_BUF_BYTES);
    CU_ASSERT_EQUAL(hda_sd_read16(HDA_SD_LVI), HDA_BDL_ENTRIES - 1);
    CU_ASSERT_EQUAL(hda_sd_read16(HDA_SD_FMT), HDA_FMT_48K_16BIT_STEREO);

    /* The BDL pointer must name the driver's own BDL page, split high/low. */
    CU_ASSERT_NOT_EQUAL(hda_bdl_phys(), 0);
    CU_ASSERT_EQUAL(hda_bdl_phys() & 0x7F, 0);   /* 128-byte aligned         */
    CU_ASSERT_EQUAL(hda_sd_read32(HDA_SD_BDPL), (uint32_t)hda_bdl_phys());
    CU_ASSERT_EQUAL(hda_sd_read32(HDA_SD_BDPU),
                    (uint32_t)(hda_bdl_phys() >> 32));

    /* Interrupt routing: the master enable plus this descriptor's own bit.
     * Without the per-stream bit, IOC fires into a status register nobody is
     * told about. */
    uint32_t intctl = hda_reg_read32(HDA_REG_INTCTL);
    CU_ASSERT(intctl & HDA_INTCTL_GIE);
    CU_ASSERT(intctl & (1u << hda_stream_index()));

    /* A descriptor error would already be latched by now if the BDL were
     * malformed, and it is cheaper to catch here than in the DMA test. */
    CU_ASSERT_EQUAL(hda_sd_status() & (HDA_SDSTS_FIFOE | HDA_SDSTS_DESE), 0);
}

static void test_dma_runs_and_completion_irq_fires(void)
{
    /*
     * The heart of the ticket. Everything above proves registers hold the
     * right values; this proves the hardware acted on them.
     *
     * SDnLPIB is the controller's own byte position in the cyclic buffer,
     * updated as it reads. It cannot advance unless bus mastering is on, the
     * BDL entries name readable physical memory, and the stream is running --
     * so an advancing LPIB is what separates "programmed" from "playing".
     *
     * The interrupt count then proves the BDL's IOC flag reached the CPU:
     * the controller finished an entry, asserted INTx, the PIC delivered it
     * on the line read out of PCI config space, and irq_hda_stub ->
     * hda_irq_handler ran and cleared SDnSTS. That is the completion IRQ the
     * acceptance criteria names, taken through the existing PIC/IDT with no
     * MSI support anywhere in this kernel.
     */
    CU_ASSERT_TRUE(hda_is_playing());

    /* A usable INTx line is a precondition for the IRQ half; hda_init()
     * leaves this at 0xFF when it refused to wire one. */
    CU_ASSERT_NOT_EQUAL(hda_irq_line(), 0xFF);

    uint32_t pos_before = hda_stream_position();
    uint32_t irqs_before = hda_irq_count();

    wait_ms_with_irqs(HDA_TEST_WAIT_MS);

    uint32_t pos_after = hda_stream_position();
    uint32_t irqs_after = hda_irq_count();

    /* Position must have moved. Compared for inequality rather than
     * ordering, because the buffer is cyclic and ~400 ms is longer than one
     * loop, so "after > before" is not guaranteed even when it works. */
    CU_ASSERT_NOT_EQUAL(pos_after, pos_before);
    CU_ASSERT(pos_after < HDA_BUF_BYTES);

    /* At least one BDL entry completed. */
    CU_ASSERT(irqs_after > irqs_before);

    /* And nothing went wrong on the way: FIFOE means the DMA could not keep
     * the codec fed, DESE means the controller rejected a BDL entry. Either
     * one can coexist with a healthy-looking LPIB, so they are checked
     * separately from the position. */
    CU_ASSERT_EQUAL(hda_stream_errors(), 0);
}

static void test_tone_frequency_is_snapped_not_ignored(void)
{
    /* The buffer loops, so the wave has to close on itself: the driver
     * rounds to a whole number of periods, which quantises the frequency to
     * 48000/16384 ~= 2.93 Hz. Requesting 440 Hz must land within one step of
     * it -- a driver that ignored the argument entirely, or that got the
     * periods maths wrong by a factor, fails here while every register
     * assertion above still passes. */
    CU_ASSERT_EQUAL(hda_play_tone(HDA_TEST_HZ, 0), HDA_OK);
    uint32_t actual = hda_tone_hz();
    uint32_t step = HDA_SAMPLE_RATE_HZ / HDA_BUF_FRAMES + 1;
    CU_ASSERT(actual + step >= HDA_TEST_HZ);
    CU_ASSERT(actual <= HDA_TEST_HZ + step);
}

static void test_stop_halts_the_stream(void)
{
    hda_stop();
    CU_ASSERT_FALSE(hda_is_playing());
    CU_ASSERT_EQUAL(hda_sd_read32(HDA_SD_CTL) & HDA_SDCTL_RUN, 0);
}

static void test_rejects_out_of_range_frequency(void)
{
    /* Same contract as speaker_tone()'s SPEAKER_EINVAL: reject and leave the
     * hardware exactly as it was, rather than clamping silently or
     * programming a stream that plays nothing. The stream is stopped at this
     * point (previous test), so "untouched" is observable as still stopped. */
    CU_ASSERT_EQUAL(hda_play_tone(HDA_MIN_HZ - 1, 100), HDA_EINVAL);
    CU_ASSERT_EQUAL(hda_play_tone(HDA_MAX_HZ + 1, 100), HDA_EINVAL);
    CU_ASSERT_EQUAL(hda_play_tone(0, 100), HDA_EINVAL);

    CU_ASSERT_FALSE(hda_is_playing());
    CU_ASSERT_EQUAL(hda_sd_read32(HDA_SD_CTL) & HDA_SDCTL_RUN, 0);
}

static void test_timed_tone_ends_on_its_own(void)
{
    /*
     * hda_play_tone() is non-blocking and hda_tick() -- called from
     * irq0_handler alongside speaker_tick() -- is what ends a timed tone.
     * That is the property a future exo_sound_pcm depends on, so it is worth
     * proving rather than inferring from the speaker driver's version.
     *
     * Needs interrupts: with IF clear there is no IRQ0 and hda_tick() never
     * runs, which would make this hang rather than fail.
     */
    CU_ASSERT_EQUAL(hda_play_tone(HDA_TEST_HZ, 50), HDA_OK);
    CU_ASSERT_TRUE(hda_is_playing());

    wait_ms_with_irqs(150);

    CU_ASSERT_FALSE(hda_is_playing());
    CU_ASSERT_EQUAL(hda_sd_read32(HDA_SD_CTL) & HDA_SDCTL_RUN, 0);
}

void suite_hda_tests(CU_pSuite s)
{
    CU_add_test(s, "controller found and mapped",
                test_controller_found_and_mapped);
    CU_add_test(s, "reset completed and codec answered",
                test_reset_completed_and_codec_answered);
    CU_add_test(s, "CORB/RIRB rings running",
                test_corb_rirb_rings_running);
    CU_add_test(s, "verb round trip through CORB and RIRB",
                test_verb_round_trip);
    CU_add_test(s, "codec walk found a DAC and an output pin",
                test_codec_walk_found_an_output);
    CU_add_test(s, "output amp unmuted at the widget's full gain",
                test_output_amp_unmuted_at_full_gain);
    CU_add_test(s, "stream descriptor and BDL programmed",
                test_stream_and_bdl_programmed);
    CU_add_test(s, "BDL DMA runs and completion IRQ fires",
                test_dma_runs_and_completion_irq_fires);
    CU_add_test(s, "tone frequency snapped to the buffer length",
                test_tone_frequency_is_snapped_not_ignored);
    CU_add_test(s, "stop halts the stream",
                test_stop_halts_the_stream);
    CU_add_test(s, "out-of-range frequency rejected",
                test_rejects_out_of_range_frequency);
    CU_add_test(s, "timed tone ends from irq0_handler",
                test_timed_tone_ends_on_its_own);
}

/* Referenced by tests/kernel/test_runner.c's CU_add_suite("hda", ...). */
int suite_hda_cleanup(void)
{
    return suite_cleanup();
}
