#include "raycaster.h"
#include "math_tables.h" // Still need this for the rotation math

// Lodev's Map
int map[MAP_W * MAP_H] = {
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
  1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
  1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
  1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
  1,0,0,0,0,0,2,2,2,2,2,0,0,0,0,3,0,3,0,3,0,0,0,1,
  1,0,0,0,0,0,2,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0,1,
  1,0,0,0,0,0,2,0,0,0,2,0,0,0,0,3,0,0,0,3,0,0,0,1,
  1,0,0,0,0,0,2,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0,1,
  1,0,0,0,0,0,2,2,0,2,2,0,0,0,0,3,0,3,0,3,0,0,0,1,
  1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
  1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
  1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
  1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
  1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
  1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
  1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
  1,4,4,4,4,4,4,4,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
  1,4,0,4,0,0,0,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
  1,4,0,0,0,0,5,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
  1,4,0,4,0,0,0,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
  1,4,0,4,4,4,4,4,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
  1,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
  1,4,4,4,4,4,4,4,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1
};

player_t p;

// Helper to get absolute value
static inline int i_abs(int v) { return v < 0 ? -v : v; }

void init_game(void) {
    // Start at (22, 12)
    p.x = 22 * F_SCALE;
    p.y = 12 * F_SCALE;

    // Direction (-1, 0)
    p.dirX = -F_SCALE;
    p.dirY = 0;

    // Camera Plane (0, 0.66)
    // 0.66 * 1024 = ~676
    p.planeX = 0;
    p.planeY = 676; 
}

void move_player(int fwd_back) {
    int speed = 150; // Move speed
    // posX += dirX * moveSpeed;
    // We must scale down after multiplication because dirX is scaled
    int dx = (p.dirX * speed) >> F_SHIFT;
    int dy = (p.dirY * speed) >> F_SHIFT;

    if (fwd_back > 0) {
        // Simple collision detection (checking map[x][y])
        // We look ahead to where we want to go
        int nextX = (p.x + dx) >> F_SHIFT;
        int nextY = (p.y + dy) >> F_SHIFT;
        
        // Only move if the map cell is 0 (walkable)
        if (map[nextX * MAP_W + (p.y >> F_SHIFT)] == 0) p.x += dx;
        if (map[(p.x >> F_SHIFT) * MAP_W + nextY] == 0) p.y += dy;
    } 
    else {
        // Moving backwards
        int nextX = (p.x - dx) >> F_SHIFT;
        int nextY = (p.y - dy) >> F_SHIFT;
        
        if (map[nextX * MAP_W + (p.y >> F_SHIFT)] == 0) p.x -= dx;
        if (map[(p.x >> F_SHIFT) * MAP_W + nextY] == 0) p.y -= dy;
    }
}

void rotate_player(int dir) {
    // Rotation speed in degrees (approx)
    int rot_deg = 3; 
    if (dir < 0) rot_deg = -3;
    
    // We use our existing math_tables.h (Scale 256)
    // Warning: M_Cos/M_Sin take an angle 0-359. 
    // This is tricky because Lodev uses vectors, not angles.
    // However, rotation matrix is:
    // oldDirX = dirX;
    // dirX = dirX * cos(rot) - dirY * sin(rot);
    // dirY = oldDirX * sin(rot) + dirY * cos(rot);
    
    // Let's just grab the values from our table
    int c = M_Cos(rot_deg < 0 ? 360 + rot_deg : rot_deg);
    int s = M_Sin(rot_deg < 0 ? 360 + rot_deg : rot_deg);

    // Rotate dir vector
    int oldDirX = p.dirX;
    // Math Table is Scale 256, so we divide by 256 (>>8)
    p.dirX = (p.dirX * c - p.dirY * s) >> 8;
    p.dirY = (oldDirX * s + p.dirY * c) >> 8;

    // Rotate plane vector
    int oldPlaneX = p.planeX;
    p.planeX = (p.planeX * c - p.planeY * s) >> 8;
    p.planeY = (oldPlaneX * s + p.planeY * c) >> 8;
}

void render_raycaster(framebuffer_t* fb) {
    // 1. Draw Ceiling and Floor
    fb_fill_rect(fb, 0, 0, fb->width, fb->height/2, 20, 20, 20); // Dark Gray Ceiling
    fb_fill_rect(fb, 0, fb->height/2, fb->width, fb->height/2, 50, 50, 50); // Floor

    int w = fb->width;
    int h = fb->height;

    for (int x = 0; x < w; x++) {
        // Calculate ray position and direction
        // cameraX is -1 on left side, 0 in center, 1 on right side
        // Fixed Point: 2 * x * F_SCALE / w - F_SCALE
        int cameraX = ((2 * x * F_SCALE) / w) - F_SCALE;

        int rayDirX = p.dirX + ((p.planeX * cameraX) >> F_SHIFT);
        int rayDirY = p.dirY + ((p.planeY * cameraX) >> F_SHIFT);

        // Which box of the map we're in
        int mapX = p.x >> F_SHIFT;
        int mapY = p.y >> F_SHIFT;

        // Length of ray from current position to next x or y-side
        int sideDistX, sideDistY;

        // Delta distance calculation
        // float: abs(1 / rayDirX)
        // Fixed: abs(F_SCALE * F_SCALE / rayDirX)
        // We multiply by F_SCALE again because 1.0 is F_SCALE
        // If rayDirX is 0, set to very large number
        int deltaDistX = (rayDirX == 0) ? 2147483640 : i_abs((F_SCALE * F_SCALE) / rayDirX);
        int deltaDistY = (rayDirY == 0) ? 2147483640 : i_abs((F_SCALE * F_SCALE) / rayDirY);

        int perpWallDist;

        // Step direction
        int stepX, stepY;
        int hit = 0;
        int side; // 0 for NS, 1 for EW

        // Calculate step and initial sideDist
        if (rayDirX < 0) {
            stepX = -1;
            // (posX - mapX) is the fractional part within the tile
            // We need strictly the fractional fixed point part here
            int fracX = p.x - (mapX << F_SHIFT);
            sideDistX = (fracX * deltaDistX) >> F_SHIFT;
        } else {
            stepX = 1;
            int fracX = ((mapX + 1) << F_SHIFT) - p.x;
            sideDistX = (fracX * deltaDistX) >> F_SHIFT;
        }

        if (rayDirY < 0) {
            stepY = -1;
            int fracY = p.y - (mapY << F_SHIFT);
            sideDistY = (fracY * deltaDistY) >> F_SHIFT;
        } else {
            stepY = 1;
            int fracY = ((mapY + 1) << F_SHIFT) - p.y;
            sideDistY = (fracY * deltaDistY) >> F_SHIFT;
        }

        // --- DDA Algorithm ---
        while (hit == 0) {
            // Jump to next map square
            if (sideDistX < sideDistY) {
                sideDistX += deltaDistX;
                mapX += stepX;
                side = 0;
            } else {
                sideDistY += deltaDistY;
                mapY += stepY;
                side = 1;
            }
            
            // Check bounds to prevent crash
            if (mapX < 0 || mapX >= MAP_W || mapY < 0 || mapY >= MAP_H) {
                hit = 1; // Fake hit on map edge
                side = 0; // Prevent infinite loops
            } else if (map[mapY * MAP_W + mapX] > 0) {
                hit = 1;
            }
        }

        // Calculate distance projected on camera direction
        // Note: The Lodev tutorial formula: (sideDistX - deltaDistX)
        // This value is already scaled by F_SCALE because sideDist/deltaDist are scaled.
        if (side == 0) perpWallDist = sideDistX - deltaDistX;
        else           perpWallDist = sideDistY - deltaDistY;

        // Calculate height of line to draw on screen
        // perpWallDist is Fixed Point (scaled by 1024). 
        // We need to divide h by real distance.
        // real_dist = perpWallDist / 1024.
        // lineH = h / (perpWallDist / 1024)  ->  (h * 1024) / perpWallDist
        
        int lineH = 0;
        if (perpWallDist > 0) {
            lineH = (h * F_SCALE) / perpWallDist;
        } else {
            lineH = h; // Very close
        }
        
        // Clamp height for visual sanity
        // (Though technically we should draw off-screen for huge walls, 
        // fb_fill_rect handles clipping, so we just calculate start/end)
        
        int drawStart = -lineH / 2 + h / 2;
        int drawEnd = lineH / 2 + h / 2;
        
        // Colors from map
        int tile = 0;
        if (mapX >= 0 && mapX < MAP_W && mapY >= 0 && mapY < MAP_H) {
            tile = map[mapY * MAP_W + mapX];
        }

        uint8_t r_col=0, g_col=0, b_col=0;
        switch(tile) {
            case 1: r_col = 255; break; // Red
            case 2: g_col = 255; break; // Green
            case 3: b_col = 255; break; // Blue
            case 4: r_col = 255; g_col = 255; b_col = 255; break; // White
            default: r_col = 255; g_col = 255; break; // Yellow
        }

        // Give x and y sides different brightness (Shadowing)
        if (side == 1) {
            r_col /= 2; g_col /= 2; b_col /= 2;
        }

        // Draw the vertical stripe
        // Since we are clamping lineH, we use the raw lineH for the rect, 
        // and let the fill_rect function handle the clipping (assuming your fb_fill_rect clips safely!)
        // If your fb_fill_rect doesn't clip negative Y, we must fix drawStart:
        
        if (drawStart < 0) drawStart = 0;
        if (drawEnd >= h) drawEnd = h - 1;
        
        fb_fill_rect(fb, x, drawStart, 1, drawEnd - drawStart + 1, r_col, g_col, b_col);
    }
}