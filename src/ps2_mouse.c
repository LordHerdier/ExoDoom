#include "ps2_mouse.h"
#include "io.h"
#include "pic.h"

#include <stdint.h>

#define PS2_DATA_PORT   0x60
#define PS2_STATUS_PORT 0x64

/* Status register (port 0x64) bits -- same layout src/ps2.c uses. */
#define PS2_STATUS_OBF  0x01   /* output buffer full — a byte is waiting  */
#define PS2_STATUS_IBF  0x02   /* input buffer full — controller is still
                                 * digesting the last byte we sent it      */
#define PS2_STATUS_AUX  0x20   /* byte came from the auxiliary (mouse) port */

/* Controller command bytes (port 0x64). */
#define PS2_CMD_ENABLE_AUX    0xA8  /* enable the second (aux) PS/2 port    */
#define PS2_CMD_READ_CONFIG   0x20  /* next read from 0x60 is the config byte */
#define PS2_CMD_WRITE_CONFIG  0x60  /* next write to 0x60 is the config byte */
#define PS2_CMD_WRITE_TO_AUX  0xD4  /* next write to 0x60 goes to the aux device */

/* Controller configuration byte bits (src/ps2.c's driver never reads/writes
 * this byte, so these are declared here rather than shared). */
#define PS2_CFG_AUX_IRQ_EN    0x02  /* enable IRQ12 on aux output           */
#define PS2_CFG_AUX_CLOCK_DIS 0x20  /* 1 = aux port clock disabled          */

/* Mouse command bytes (sent via PS2_CMD_WRITE_TO_AUX). */
#define PS2_MOUSE_CMD_ENABLE_REPORTING 0xF4
#define PS2_MOUSE_ACK 0xFA

/* Bounded spins, same rationale and same magnitude as src/ps2.c's
 * PS2_POLL_SPINS / PS2_IRQ_DRAIN_MAX: hardware that never responds must not
 * hang the caller, and a wedged controller must not spin the IRQ handler
 * forever. */
#define PS2_POLL_SPINS     100000u
#define PS2_IRQ_DRAIN_MAX  32u

/* Packet-decode state: 3-byte stream, byte 0 always has bit 3 set (the
 * alignment-check bit) so a misaligned/lost byte can be detected and
 * resynced from rather than silently corrupting every packet after it. */
static uint8_t  packet_bytes[3];
static uint8_t  packet_pos = 0;

static int16_t  mouse_dx = 0;
static int16_t  mouse_dy = 0;
static uint8_t  mouse_buttons = 0;

static volatile uint32_t mouse_non_aux_bytes = 0;
static volatile uint32_t mouse_irq_overruns  = 0;
static volatile uint32_t mouse_resync_count  = 0;

/* Spin until the controller's input buffer is clear (safe to write a byte),
 * or the bound runs out -- the write is issued either way, mirroring
 * ps2_read_scancode()'s "spin, then act regardless" shape in src/ps2.c. */
static void ps2_ctrl_wait_input_clear(void) {
    for (uint32_t spins = 0; spins < PS2_POLL_SPINS; spins++) {
        if (!(inb(PS2_STATUS_PORT) & PS2_STATUS_IBF))
            return;
    }
}

/* Spin until the controller's output buffer has a byte waiting, or the bound
 * runs out -- the read is issued either way. */
static void ps2_ctrl_wait_output_full(void) {
    for (uint32_t spins = 0; spins < PS2_POLL_SPINS; spins++) {
        if (inb(PS2_STATUS_PORT) & PS2_STATUS_OBF)
            return;
    }
}

static void ps2_ctrl_write_command(uint8_t cmd) {
    ps2_ctrl_wait_input_clear();
    outb(PS2_STATUS_PORT, cmd);
}

static void ps2_ctrl_write_data(uint8_t data) {
    ps2_ctrl_wait_input_clear();
    outb(PS2_DATA_PORT, data);
}

static uint8_t ps2_ctrl_read_data(void) {
    ps2_ctrl_wait_output_full();
    return inb(PS2_DATA_PORT);
}

/*
 * ps2_mouse_init — Standard PS/2 controller aux-port bring-up.
 *
 *   1. 0xA8 to the command port enables the second (aux) PS/2 port -- most
 *      controllers power it up disabled.
 *   2. Read-modify-write the controller's configuration byte: set the
 *      "enable IRQ12" bit and clear the "aux clock disabled" bit, leaving
 *      every other bit (translation, IRQ1 enable, ...) exactly as the
 *      firmware/kbd_init() left it.
 *   3. 0xD4 routes the next data-port byte to the aux device instead of the
 *      keyboard; 0xF4 there is the mouse's own "enable data reporting"
 *      command. The mouse ACKs with 0xFA, which is read and discarded --
 *      there is nothing to retry onto if it doesn't (matches kbd_init()'s
 *      "warn, don't loop" stance on a wedged controller).
 */
void ps2_mouse_init(void) {
    ps2_mouse_reset();

    ps2_ctrl_write_command(PS2_CMD_ENABLE_AUX);

    ps2_ctrl_write_command(PS2_CMD_READ_CONFIG);
    uint8_t cfg = ps2_ctrl_read_data();
    cfg |= PS2_CFG_AUX_IRQ_EN;
    cfg &= (uint8_t)~PS2_CFG_AUX_CLOCK_DIS;
    ps2_ctrl_write_command(PS2_CMD_WRITE_CONFIG);
    ps2_ctrl_write_data(cfg);

    ps2_ctrl_write_command(PS2_CMD_WRITE_TO_AUX);
    ps2_ctrl_write_data(PS2_MOUSE_CMD_ENABLE_REPORTING);
    (void)ps2_ctrl_read_data(); /* expected 0xFA; nothing to do differently
                                 * on a timeout or a resend/error byte -- the
                                 * mouse simply stays silent and IRQ12 never
                                 * fires, same failure mode as a keyboard
                                 * that never got plugged in. */
}

/*
 * ps2_mouse_process_byte — Decode one byte of the 3-byte packet stream.
 *
 * Byte 0: bit0 left button, bit1 right, bit2 middle, bit3 always 1 (the
 *         alignment check), bit4 X sign, bit5 Y sign, bit6 X overflow,
 *         bit7 Y overflow.
 * Byte 1: X movement magnitude (0-255, sign from byte 0 bit 4).
 * Byte 2: Y movement magnitude (0-255, sign from byte 0 bit 5).
 *
 * Runs in interrupt context (IRQ12), so like ps2_process_scancode() it does
 * no serial I/O.
 */
void ps2_mouse_process_byte(uint8_t byte) {
    if (packet_pos == 0) {
        if (!(byte & 0x08)) {
            /* Not the start of a packet -- desynced. Drop it and wait for a
             * byte that does carry the alignment bit rather than building a
             * packet out of the wrong three bytes. */
            mouse_resync_count++;
            return;
        }
        packet_bytes[0] = byte;
        packet_pos = 1;
        return;
    }

    packet_bytes[packet_pos++] = byte;
    if (packet_pos < 3)
        return;
    packet_pos = 0;

    uint8_t b0 = packet_bytes[0];
    int dx = packet_bytes[1];
    int dy = packet_bytes[2];

    if (b0 & 0x10) dx -= 256;   /* sign-extend X */
    if (b0 & 0x20) dy -= 256;   /* sign-extend Y */

    /* Overflow means this axis's reported movement is garbage -- drop it to
     * 0 for this packet rather than applying a corrupt delta. */
    if (b0 & 0x40) dx = 0;
    if (b0 & 0x80) dy = 0;

    int32_t sum_x = (int32_t)mouse_dx + dx;
    int32_t sum_y = (int32_t)mouse_dy + dy;

    if (sum_x > INT16_MAX) sum_x = INT16_MAX;
    if (sum_x < INT16_MIN) sum_x = INT16_MIN;
    if (sum_y > INT16_MAX) sum_y = INT16_MAX;
    if (sum_y < INT16_MIN) sum_y = INT16_MIN;

    mouse_dx = (int16_t)sum_x;
    mouse_dy = (int16_t)sum_y;

    /* Buttons are a level, not a delta -- always overwritten, never OR'd. */
    mouse_buttons = b0 & 0x07;
}

void ps2_mouse_poll(ps2_mouse_state_t *out) {
    out->dx = mouse_dx;
    out->dy = mouse_dy;
    out->buttons = mouse_buttons;

    mouse_dx = 0;
    mouse_dy = 0;
}

void ps2_mouse_reset(void) {
    packet_pos = 0;
    mouse_dx = 0;
    mouse_dy = 0;
    mouse_buttons = 0;
    mouse_non_aux_bytes = 0;
    mouse_irq_overruns = 0;
    mouse_resync_count = 0;
}

uint32_t ps2_mouse_non_aux_count(void) {
    return mouse_non_aux_bytes;
}

uint32_t ps2_mouse_irq_overrun_count(void) {
    return mouse_irq_overruns;
}

uint32_t ps2_mouse_resync_count(void) {
    return mouse_resync_count;
}

/*
 * ps2_mouse_irq12_handler — IRQ12 entry point.
 *
 * Mirrors ps2_irq1_handler's bounded-drain shape (src/ps2.c): the output
 * buffer is shared with the keyboard, so a byte the controller did not tag
 * AUX belongs to IRQ1 and must not be fed to the packet decoder.
 */
void ps2_mouse_irq12_handler(void) {
    unsigned i;

    for (i = 0; i < PS2_IRQ_DRAIN_MAX; i++) {
        uint8_t status = inb(PS2_STATUS_PORT);

        if (!(status & PS2_STATUS_OBF))
            break;

        uint8_t data = inb(PS2_DATA_PORT);

        if (!(status & PS2_STATUS_AUX)) {
            mouse_non_aux_bytes++;
            continue;
        }

        ps2_mouse_process_byte(data);
    }

    if (i == PS2_IRQ_DRAIN_MAX && (inb(PS2_STATUS_PORT) & PS2_STATUS_OBF))
        mouse_irq_overruns++;

    pic_send_EOI(12);
}

void irq12_handler(void) {
    ps2_mouse_irq12_handler();
}
