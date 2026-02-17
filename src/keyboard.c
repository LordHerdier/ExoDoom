#include "keyboard.h"
#include "io.h"
#include <stdbool.h>

#define PS2_DATA_PORT    0x60
#define PS2_STATUS_PORT  0x64

static bool is_extended = false; // State to remember if we saw 0xE0

// Standard ASCII Map (Scancode Set 1)
static const char scancode_map[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

void keyboard_init(void) {
    while (inb(PS2_STATUS_PORT) & 1) inb(PS2_DATA_PORT);
    is_extended = false;
}

char keyboard_poll(void) {
    uint8_t status = inb(PS2_STATUS_PORT);

    if (status & 1) {
        uint8_t scancode = inb(PS2_DATA_PORT);

        // 1. Handle Extended Byte Prefix (0xE0)
        if (scancode == 0xE0) {
            is_extended = true;
            return 0; // Not a complete key yet, return nothing
        }

        // 2. Handle Key Release (Bit 7 set)
        if (scancode & 0x80) {
            is_extended = false; // Reset state on release
            return 0;
        }

        // 3. Handle Extended Keys (Arrows)
        if (is_extended) {
            is_extended = false; // Reset state
            switch (scancode) {
                case 0x48: return KEY_UP;
                case 0x4B: return KEY_LEFT;
                case 0x4D: return KEY_RIGHT;
                case 0x50: return KEY_DOWN;
                default: return 0; // Unknown extended key
            }
        }
        
        // 4. Handle Standard ASCII
        else {
            if (scancode < 128) {
                return scancode_map[scancode];
            }
        }
    }
    return 0;
}