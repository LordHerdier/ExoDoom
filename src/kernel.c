#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

static inline void put_px32(uint8_t* fb, uint32_t pitch, uint32_t x, uint32_t y, uint32_t px) {
    *(uint32_t*)(fb + y * pitch + x * 4) = px;
}

// Based on your byte-lane probe: [B][G][R][X] in memory
static inline uint32_t pack_bgrx8888(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b; // 0x00RRGGBB
}

static void fill_rect32(uint8_t* fb, uint32_t pitch,
                        uint32_t x0, uint32_t y0, uint32_t w, uint32_t h,
                        uint32_t px) {
    for (uint32_t y = 0; y < h; y++) {
        uint32_t* row = (uint32_t*)(fb + (y0 + y) * pitch + x0 * 4);
        for (uint32_t x = 0; x < w; x++) row[x] = px;
    }
}

static void framebuffer_color_sanity(uint8_t* fb, uint32_t pitch, uint32_t w, uint32_t h) {
    // Clear
    fill_rect32(fb, pitch, 0, 0, w, h, pack_bgrx8888(0,0,0));

    // ---- Top: 8 color bars (should look exactly like: W Y C G M R B K) ----
    const uint32_t bars[8] = {
        pack_bgrx8888(255,255,255), // white
        pack_bgrx8888(255,255,0),   // yellow
        pack_bgrx8888(0,255,255),   // cyan
        pack_bgrx8888(0,255,0),     // green
        pack_bgrx8888(255,0,255),   // magenta
        pack_bgrx8888(255,0,0),     // red
        pack_bgrx8888(0,0,255),     // blue
        pack_bgrx8888(0,0,0)        // black
    };

    uint32_t bar_h = h / 5;
    if (bar_h < 60) bar_h = (h < 60) ? h : 60;
    uint32_t bar_w = w / 8;

    for (uint32_t i = 0; i < 8; i++) {
        uint32_t x = i * bar_w;
        uint32_t ww = (i == 7) ? (w - x) : bar_w;
        fill_rect32(fb, pitch, x, 0, ww, bar_h, bars[i]);
    }

    // ---- Middle-left: grayscale ramp (should go black -> white smoothly) ----
    uint32_t y0 = bar_h + 10;
    uint32_t ramp_h = (h - y0) / 2;
    uint32_t ramp_w = w / 2;

    for (uint32_t y = 0; y < ramp_h; y++) {
        for (uint32_t x = 0; x < ramp_w; x++) {
            uint8_t t = (uint8_t)((x * 255u) / (ramp_w ? (ramp_w - 1) : 1));
            put_px32(fb, pitch, x, y0 + y, pack_bgrx8888(t,t,t));
        }
    }

    // ---- Middle-right: pure RGB blocks + white (should be unmistakable) ----
    uint32_t bx = ramp_w + 10;
    uint32_t by = y0;
    uint32_t sq = (ramp_h / 2) - 5;
    if (sq > 120) sq = 120;

    fill_rect32(fb, pitch, bx,         by,         sq, sq, pack_bgrx8888(255,0,0));   // R
    fill_rect32(fb, pitch, bx + sq+10, by,         sq, sq, pack_bgrx8888(0,255,0));   // G
    fill_rect32(fb, pitch, bx,         by + sq+10, sq, sq, pack_bgrx8888(0,0,255));   // B
    fill_rect32(fb, pitch, bx + sq+10, by + sq+10, sq, sq, pack_bgrx8888(255,255,255)); // W

    // ---- Bottom: tiny RGB cube sampler (6x6 grid) ----
    uint32_t grid_y = y0 + ramp_h + 10;
    uint32_t grid_h = h - grid_y;
    if (grid_h > 0) {
        uint32_t cells = 6;
        uint32_t cell = (w / (cells + 2));
        if (cell < 20) cell = 20;
        uint32_t gx = 10;

        for (uint32_t gy = 0; gy < cells; gy++) {
            for (uint32_t gx2 = 0; gx2 < cells; gx2++) {
                // R increases left->right, G increases top->bottom, B is a fixed “stripe” per column group
                uint8_t r = (uint8_t)((gx2 * 255u) / (cells - 1));
                uint8_t g = (uint8_t)((gy  * 255u) / (cells - 1));
                uint8_t b = (uint8_t)(((gx2 ^ gy) * 255u) / (cells - 1));
                uint32_t px = pack_bgrx8888(r,g,b);

                uint32_t x0 = gx + gx2 * (cell + 4);
                uint32_t y1 = grid_y + gy  * (cell + 4);
                if (x0 + cell < w && y1 + cell < h)
                    fill_rect32(fb, pitch, x0, y1, cell, cell, px);
            }
        }
    }
}

static inline uint32_t mask_nbits(uint8_t n) {
    if (n >= 32) return 0xFFFFFFFFu;
    if (n == 0)  return 0u;
    return (1u << n) - 1u;
}

static inline uint32_t scale_8_to_n(uint8_t v8, uint8_t nbits) {
    if (nbits == 0) return 0;
    uint32_t maxn = mask_nbits(nbits);
    return (uint32_t)((v8 * maxn + 127) / 255);
}

static inline uint32_t pack_rgb_from_mb(uint8_t r8, uint8_t g8, uint8_t b8,
                                        const uint8_t color_info[6]) {
    // Multiboot1 RGB framebuffer color_info layout:
    // [0]=red_position, [1]=red_mask_size,
    // [2]=green_position, [3]=green_mask_size,
    // [4]=blue_position, [5]=blue_mask_size
    uint8_t rp = color_info[0], rs = color_info[1];
    uint8_t gp = color_info[2], gs = color_info[3];
    uint8_t bp = color_info[4], bs = color_info[5];

    uint32_t r = scale_8_to_n(r8, rs) & mask_nbits(rs);
    uint32_t g = scale_8_to_n(g8, gs) & mask_nbits(gs);
    uint32_t b = scale_8_to_n(b8, bs) & mask_nbits(bs);

    return (r << rp) | (g << gp) | (b << bp);
}

static inline uint32_t pack_mb_rgb(uint8_t r8, uint8_t g8, uint8_t b8, const uint8_t ci[6]) {
    // ci = { r_pos, r_size, g_pos, g_size, b_pos, b_size }
    uint8_t rp = ci[0], rs = ci[1];
    uint8_t gp = ci[2], gs = ci[3];
    uint8_t bp = ci[4], bs = ci[5];

    uint32_t r = scale_8_to_n(r8, rs) & mask_nbits(rs);
    uint32_t g = scale_8_to_n(g8, gs) & mask_nbits(gs);
    uint32_t b = scale_8_to_n(b8, bs) & mask_nbits(bs);

    return (r << rp) | (g << gp) | (b << bp);
}


static void draw_hline32(uint8_t* fb, uint32_t pitch, uint32_t x0, uint32_t x1, uint32_t y, uint32_t px) {
    for (uint32_t x = x0; x <= x1; x++) put_px32(fb, pitch, x, y, px);
}

static void draw_vline32(uint8_t* fb, uint32_t pitch, uint32_t x, uint32_t y0, uint32_t y1, uint32_t px) {
    for (uint32_t y = y0; y <= y1; y++) put_px32(fb, pitch, x, y, px);
}

static inline uint32_t pack_xrgb8888(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void fb_fill(uint8_t* fb, uint32_t pitch, uint32_t w, uint32_t h, uint32_t px) {
    for (uint32_t y = 0; y < h; y++) {
        uint32_t* row = (uint32_t*)(fb + y * pitch);
        for (uint32_t x = 0; x < w; x++) row[x] = px;
    }
}

static void fb_test_pixel_format(uint8_t* fb, uint32_t pitch, uint32_t w, uint32_t h, const uint8_t ci[6]) {
    fb_fill(fb, pitch, w, h, 0);

    // ---- (A) Byte-lane probe: which byte controls which visible channel? ----
    // Each bar sets one byte (in memory) to 0xFF.
    // If you see a BLUE bar for "byte0", that means lowest-address byte is blue, etc.
    uint32_t bar_h = (h > 120) ? 120 : (h / 4);
    if (bar_h < 40) bar_h = (h / 4);

    for (uint32_t y = 0; y < bar_h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint32_t q = x / (w / 4 ? (w / 4) : 1);
            if (q > 3) q = 3;

            uint32_t px = 0;
            // Set a single byte lane to 0xFF
            px = (q == 0) ? 0x000000FFu :
                 (q == 1) ? 0x0000FF00u :
                 (q == 2) ? 0x00FF0000u :
                            0xFF000000u;

            put_px32(fb, pitch, x, y, px);
        }
    }

    // ---- (B) Split-screen comparison ----
    // Left half: Multiboot color_info pack
    // Right half: xRGB8888 pack
    uint32_t y0 = bar_h + 4;
    if (y0 >= h) return;

    for (uint32_t y = y0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            // Make a smooth gradient field:
            // R = x, G = y, B = x^y (gives diagonals / checks for swaps)
            uint8_t r = (uint8_t)((x * 255u) / (w ? (w - 1) : 1));
            uint8_t g = (uint8_t)(((y - y0) * 255u) / ((h - y0) ? (h - y0 - 1) : 1));
            uint8_t b = (uint8_t)(r ^ g);

            uint32_t px = (x < w/2)
                ? pack_mb_rgb(r, g, b, ci)
                : pack_xrgb8888(r, g, b);

            put_px32(fb, pitch, x, y, px);
        }
    }

    // ---- (C) Corner reference squares (easy human eyeballing) ----
    // top-left-ish under probe: MB packed primaries
    uint32_t sq = 50;
    uint32_t ox = 10, oy = (bar_h > 10) ? (bar_h - 10) : 0;
    if (oy + sq < h && ox + sq*4 < w) {
        uint32_t Rm = pack_mb_rgb(255,0,0,ci);
        uint32_t Gm = pack_mb_rgb(0,255,0,ci);
        uint32_t Bm = pack_mb_rgb(0,0,255,ci);
        uint32_t Wm = pack_mb_rgb(255,255,255,ci);
        for (uint32_t y = 0; y < sq; y++) {
            for (uint32_t x = 0; x < sq; x++) put_px32(fb,pitch, ox + x + sq*0, oy + y, Rm);
            for (uint32_t x = 0; x < sq; x++) put_px32(fb,pitch, ox + x + sq*1, oy + y, Gm);
            for (uint32_t x = 0; x < sq; x++) put_px32(fb,pitch, ox + x + sq*2, oy + y, Bm);
            for (uint32_t x = 0; x < sq; x++) put_px32(fb,pitch, ox + x + sq*3, oy + y, Wm);
        }
    }
}

static void framebuffer_test32(uint8_t* fb, uint32_t pitch, uint32_t w, uint32_t h,
                               const uint8_t color_info[6]) {
    // Colors packed using real MB bit positions/sizes:
    uint32_t BLACK   = pack_rgb_from_mb(0,   0,   0,   color_info);
    uint32_t WHITE   = pack_rgb_from_mb(255, 255, 255, color_info);
    uint32_t RED     = pack_rgb_from_mb(255, 0,   0,   color_info);
    uint32_t GREEN   = pack_rgb_from_mb(0,   255, 0,   color_info);
    uint32_t BLUE    = pack_rgb_from_mb(0,   0,   255, color_info);
    uint32_t CYAN    = pack_rgb_from_mb(0,   255, 255, color_info);
    uint32_t MAGENTA = pack_rgb_from_mb(255, 0,   255, color_info);
    uint32_t YELLOW  = pack_rgb_from_mb(255, 255, 0,   color_info);

    // 0) Clear to black
    fill_rect32(fb, pitch, 0, 0, w, h, BLACK);

    // 1) Border + diagonals (catches pitch/stride and coordinate errors fast)
    draw_hline32(fb, pitch, 0, w-1, 0, WHITE);
    draw_hline32(fb, pitch, 0, w-1, h-1, WHITE);
    draw_vline32(fb, pitch, 0, 0, h-1, WHITE);
    draw_vline32(fb, pitch, w-1, 0, h-1, WHITE);

    uint32_t diag_len = (w < h) ? w : h;
    for (uint32_t i = 0; i < diag_len; i++) {
        put_px32(fb, pitch, i, i, GREEN);
        put_px32(fb, pitch, (w - 1) - i, i, RED);
    }

    // Layout: top 40% = color bars, middle 30% = gradients, bottom 30% = checker + gamma-ish ramp
    uint32_t y0 = 1;
    uint32_t y_color_end = (h * 40) / 100;
    uint32_t y_grad_end  = (h * 70) / 100;

    // 2) Color bars (8 vertical bars)
    const uint32_t bars[8] = { WHITE, YELLOW, CYAN, GREEN, MAGENTA, RED, BLUE, BLACK };
    uint32_t bar_w = w / 8;
    for (uint32_t b = 0; b < 8; b++) {
        uint32_t x = b * bar_w;
        uint32_t ww = (b == 7) ? (w - x) : bar_w;
        fill_rect32(fb, pitch, x, y0, ww, (y_color_end - y0), bars[b]);
    }

    // 3) Gradients: left=grayscale, middle=red, right=green+blue combined (detect channel swap)
    uint32_t gy0 = y_color_end;
    uint32_t gy1 = y_grad_end;

    for (uint32_t y = gy0; y < gy1; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint8_t t = (uint8_t)((x * 255u) / (w - 1));
            uint32_t px;

            if (x < w/3) {
                // grayscale ramp
                px = pack_rgb_from_mb(t, t, t, color_info);
            } else if (x < (2*w)/3) {
                // red ramp
                px = pack_rgb_from_mb(t, 0, 0, color_info);
            } else {
                // green/blue ramp (if swapped you'll see it immediately)
                px = pack_rgb_from_mb(0, t, (uint8_t)(255 - t), color_info);
            }
            put_px32(fb, pitch, x, y, px);
        }
    }

    // 4) Checkerboard + small color squares (catches pixel packing + stride issues)
    uint32_t by0 = y_grad_end;
    uint32_t by1 = h - 1;

    uint32_t cell = 16;
    for (uint32_t y = by0; y < by1; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint32_t cx = (x / cell) & 1u;
            uint32_t cy = ((y - by0) / cell) & 1u;
            uint32_t px = (cx ^ cy) ? pack_rgb_from_mb(40, 40, 40, color_info)
                                    : pack_rgb_from_mb(10, 10, 10, color_info);
            put_px32(fb, pitch, x, y, px);
        }
    }

    // 5) Bottom-left: 2x2 “truth table” for channels
    uint32_t sq = 60;
    if (w > sq*2 + 10 && h > sq + 10) {
        uint32_t ox = 10;
        uint32_t oy = h - sq - 10;

        fill_rect32(fb, pitch, ox,       oy,       sq, sq, RED);
        fill_rect32(fb, pitch, ox + sq,  oy,       sq, sq, GREEN);
        fill_rect32(fb, pitch, ox,       oy + sq,  sq, sq, BLUE);
        fill_rect32(fb, pitch, ox + sq,  oy + sq,  sq, sq, WHITE);

        // outlines
        draw_hline32(fb, pitch, ox, ox + sq*2 - 1, oy, WHITE);
        draw_hline32(fb, pitch, ox, ox + sq*2 - 1, oy + sq*2 - 1, WHITE);
        draw_vline32(fb, pitch, ox, oy, oy + sq*2 - 1, WHITE);
        draw_vline32(fb, pitch, ox + sq*2 - 1, oy, oy + sq*2 - 1, WHITE);
    }
}

// ---------- Multiboot1 info ----------

struct multiboot_info {
    uint32_t flags;

    uint32_t mem_lower;
    uint32_t mem_upper;

    uint32_t boot_device;
    uint32_t cmdline;

    uint32_t mods_count;
    uint32_t mods_addr;

    uint32_t syms[4];

    uint32_t mmap_length;
    uint32_t mmap_addr;

    uint32_t drives_length;
    uint32_t drives_addr;

    uint32_t config_table;

    uint32_t boot_loader_name;

    uint32_t apm_table;

    // VBE fields
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;

    // Framebuffer fields (Multiboot1 “framebuffer” extension)
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    uint8_t  color_info[6];
} __attribute__((packed));

// ---------- Minimal COM1 serial console ----------

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static void serial_init(void) {
    outb(0x3F8 + 1, 0x00); // Disable interrupts
    outb(0x3F8 + 3, 0x80); // Enable DLAB
    outb(0x3F8 + 0, 0x03); // Divisor low  (38400 baud)
    outb(0x3F8 + 1, 0x00); // Divisor high
    outb(0x3F8 + 3, 0x03); // 8N1
    outb(0x3F8 + 2, 0xC7); // FIFO
    outb(0x3F8 + 4, 0x0B); // IRQs enabled, RTS/DSR set
}

static int serial_can_tx(void) {
    return inb(0x3F8 + 5) & 0x20;
}
static void serial_putc(char c) {
    while (!serial_can_tx()) {}
    outb(0x3F8, (uint8_t)c);
}
static void serial_print(const char* s) {
    while (*s) serial_putc(*s++);
}

// ---------- Minimal VGA text console ----------

enum vga_color {
    VGA_COLOR_BLACK = 0,
    VGA_COLOR_LIGHT_GREY = 7,
    VGA_COLOR_LIGHT_RED = 12,
};

static inline uint8_t vga_entry_color(enum vga_color fg, enum vga_color bg) {
    return fg | bg << 4;
}

static inline uint16_t vga_entry(unsigned char uc, uint8_t color) {
    return (uint16_t)uc | (uint16_t)color << 8;
}

#define VGA_WIDTH   80
#define VGA_HEIGHT  25
#define VGA_MEMORY  0xB8000

static size_t terminal_row;
static size_t terminal_column;
static uint8_t terminal_color;
static uint16_t* const terminal_buffer = (uint16_t*)VGA_MEMORY;

static void terminal_initialize(void) {
    terminal_row = 0;
    terminal_column = 0;
    terminal_color = vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);

    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            const size_t index = y * VGA_WIDTH + x;
            terminal_buffer[index] = vga_entry(' ', terminal_color);
        }
    }
}

static void terminal_putchar(char c) {
    if (c == '\n') {
        terminal_row++;
        terminal_column = 0;
        return;
    }

    const size_t index = terminal_row * VGA_WIDTH + terminal_column;
    terminal_buffer[index] = vga_entry(c, terminal_color);

    if (++terminal_column == VGA_WIDTH) {
        terminal_column = 0;
        if (++terminal_row == VGA_HEIGHT)
            terminal_row = 0;
    }
}

static void terminal_writestring(const char* s) {
    while (*s) {
        terminal_putchar(*s++);
    }
}

static void print_hex32(uint32_t v) {
    const char* hex = "0123456789ABCDEF";
    for (int i = 7; i >= 0; --i) {
        uint8_t nib = (v >> (i * 4)) & 0xF;
        terminal_putchar(hex[nib]);
    }
}

static void print_hex64(uint64_t v) {
    print_hex32((uint32_t)(v >> 32));
    terminal_putchar('_');
    print_hex32((uint32_t)(v & 0xFFFFFFFFu));
}

static void print_dec(uint32_t v) {
    char buf[16];
    int i = 0;
    if (v == 0) {
        terminal_putchar('0');
        return;
    }
    while (v > 0 && i < 15) {
        buf[i++] = '0' + (v % 10);
        v /= 10;
    }
    while (i--) {
        terminal_putchar(buf[i]);
    }
}

// ---------- kernel_main ----------

void kernel_main(uint32_t mb_info_addr)
{
    serial_init(); serial_print("hello from kernel\n"); // Debug message to COM1


    struct multiboot_info* mb = (struct multiboot_info*)mb_info_addr;

    terminal_initialize();
    terminal_writestring("ExoDoom framebuffer debug\n\n");

    terminal_writestring("flags = 0x");
    print_hex32(mb->flags);
    terminal_putchar('\n');

    if (!(mb->flags & (1u << 12))) {
        terminal_writestring("bit 12 NOT set (no framebuffer info)\n");
        for(;;);
    }
    framebuffer_color_sanity(
        (uint8_t*)(uintptr_t)mb->framebuffer_addr,
        mb->framebuffer_pitch,
        mb->framebuffer_width,
        mb->framebuffer_height
    );
    for(;;);

    terminal_writestring("bit 12 set (framebuffer info present)\n\n");

    terminal_writestring("framebuffer_addr = 0x");
    print_hex64(mb->framebuffer_addr);
    terminal_putchar('\n');

    terminal_writestring("pitch            = ");
    print_dec(mb->framebuffer_pitch);
    terminal_putchar('\n');

    terminal_writestring("width            = ");
    print_dec(mb->framebuffer_width);
    terminal_putchar('\n');

    terminal_writestring("height           = ");
    print_dec(mb->framebuffer_height);
    terminal_putchar('\n');

    terminal_writestring("bpp              = ");
    print_dec(mb->framebuffer_bpp);
    terminal_putchar('\n');

    terminal_writestring("type             = ");
    print_dec(mb->framebuffer_type);
    terminal_putchar('\n');

    for(;;);
}
