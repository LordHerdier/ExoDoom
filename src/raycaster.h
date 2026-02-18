#pragma once
#include <stdint.h>
#include "fb.h"

#define MAP_W 24
#define MAP_H 24
#define F_SCALE 1024  // Scaling factor (1.0 = 1024)
#define F_SHIFT 10    // Bit shift for scaling

typedef struct {
    int x;      // X position (Fixed Point)
    int y;      // Y position (Fixed Point)
    
    int dirX;   // Direction Vector X (Fixed Point)
    int dirY;   // Direction Vector Y (Fixed Point)
    
    int planeX; // Camera Plane X (Fixed Point)
    int planeY; // Camera Plane Y (Fixed Point)
} player_t;

void init_game(void);
void render_raycaster(framebuffer_t* fb);

// New input functions for Vector rotation
void move_player(int fwd_back);  // 1 = forward, -1 = back
void strafe_player(int left_right); // -1 = left, 1 = right (Optional, Lodev supports it easily)
void rotate_player(int dir);     // -1 = left, 1 = right