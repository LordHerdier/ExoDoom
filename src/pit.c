#include "pit.h"
#include "io.h"
#include "serial.h"
#include "pic.h"

static volatile uint64_t ticks = 0;
static uint32_t frequency = 100;

void pitInit(uint32_t hz){
    frequency = hz;
    uint32_t divisor = 1193180 / hz;

    outb(0x43, 0x36);

    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
}

uint64_t timerTicks(){
    return ticks;
}

uint64_t timerMS(){
    return (ticks * 1000) / frequency;
}

void irq0_handler(){
    ticks++;

    //print once per second
    if (ticks % frequency == 0){
        serial_print("tick: ");

        char buf[32];
        int i = 0;
        uint64_t t = ticks;

        //int to string
        char tmp[32];
        int j = 0;

        if (t == 0){
            tmp[j++] = '0';
        } else {
            while (t > 0){
                tmp[j++] = '0' + (t % 10);
                t /= 10;
            }
        }

        while (j > 0){
            buf[i++] = tmp[--j];
        }

        serial_print(buf);
        serial_print("\n");
    }

    picSendEOI(0);
}