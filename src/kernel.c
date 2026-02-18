#include <stdint.h>
#include "multiboot.h"
#include "serial.h"
#include "fb.h"
#include "fb_console.h"
#include "keyboard.h"
#include "raycaster.h" // <--- Include the new engine

void kernel_main(uint32_t mb_info_addr) {
    serial_init();
    
    struct multiboot_info* mb = (struct multiboot_info*)mb_info_addr;
    if (!(mb->flags & MULTIBOOT_INFO_FLAG_FRAMEBUFFER)) for(;;);

    framebuffer_t fb;
    if (!fb_init_bgrx8888(&fb, (uintptr_t)mb->framebuffer_addr, mb->framebuffer_pitch,
                          mb->framebuffer_width, mb->framebuffer_height, mb->framebuffer_bpp)) {
        for(;;);
    }

    fb_console_t con;
    if (!fbcon_init(&con, &fb)) for(;;);

    keyboard_init();

    fbcon_write(&con, "Lodev Raycaster Loaded.\n");
    fbcon_write(&con, "Controls: Arrows to Move/Rotate.\n");

    init_game(); // Lodev init

    while (1) {
        // Render
        render_raycaster(&fb);

    // 2. Display the Frame (The "Swap")
        fb_swap_buffers(&fb);

        // Input
        char c = keyboard_poll();
        if (c != 0) {
            if (c == 'w' || c == KEY_UP)    move_player(1);
            if (c == 's' || c == KEY_DOWN)  move_player(-1);
            
            // Note: In Lodev, Left/Right rotates the camera
            if (c == 'a' || c == KEY_LEFT)  rotate_player(-1);
            if (c == 'd' || c == KEY_RIGHT) rotate_player(1);
        }
    }
}