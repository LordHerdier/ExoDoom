#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "fb.h"

typedef struct {
    framebuffer_t* fb;

    uint32_t cols;
    uint32_t rows;

    uint32_t cursor_x; // in character cells
    uint32_t cursor_y;

    uint8_t fg_r, fg_g, fg_b;
    uint8_t bg_r, bg_g, bg_b;

    bool show_cursor;
} fb_console_t;

// Initialize a text console on a framebuffer.
// Uses an 8x16 cell (8x8 font doubled vertically).
bool fbcon_init(fb_console_t* con, framebuffer_t* fb);

void fbcon_set_color(fb_console_t* con,
                     uint8_t fg_r, uint8_t fg_g, uint8_t fg_b,
                     uint8_t bg_r, uint8_t bg_g, uint8_t bg_b);

void fbcon_clear(fb_console_t* con);

void fbcon_putc(fb_console_t* con, char c);
void fbcon_write(fb_console_t* con, const char* s);

void fbcon_enable_cursor(fb_console_t* con, bool enable);
void fbcon_redraw_cursor(fb_console_t* con);
