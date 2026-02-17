#pragma once
#include <stdint.h>

// Special Key Definitions (using values outside standard ASCII range)
#define KEY_UP      0x80
#define KEY_DOWN    0x81
#define KEY_LEFT    0x82
#define KEY_RIGHT   0x83

void keyboard_init(void);
char keyboard_poll(void);