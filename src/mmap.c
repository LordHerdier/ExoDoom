#include <stdint.h>
#include "mmap.h"
#include "serial.h"

static void serial_print_dec(uint32_t num) {
    char buf[16];
    int i = 0;

    if (num == 0) {
        serial_putc('0');
        return;
    }

    while (num > 0) {
        buf[i++] = '0' + (num % 10);
        num /= 10;
    }

    while (i > 0) {
        serial_putc(buf[--i]);
    }
}

static void serial_print_hex64(uint64_t num) {
    char hex[] = "0123456789ABCDEF";
    char buf[17];
    buf[16] = '\0';

    for (int i = 15; i >= 0; i--) {
        buf[i] = hex[num & 0xF];
        num >>= 4;
    }

    serial_print(buf);
}

static const char* region_type_name(uint32_t type) {
    if (type == 1) return "usable";
    return "reserved";
}

void mmap_print(struct multiboot_info* mb) {
    if (!(mb->flags & MULTIBOOT_INFO_FLAG_MMAP)) {
        serial_print("No multiboot mmap available\n");
        return;
    }

    serial_print("Multiboot memory map:\n");

    uintptr_t cur = (uintptr_t)mb->mmap_addr;
    uintptr_t end = cur + mb->mmap_length;

    while (cur < end) {
        struct multiboot_mmap_entry* entry =
            (struct multiboot_mmap_entry*)cur;

        serial_print(" base=0x");
        serial_print_hex64(entry->addr);

        serial_print(" len=0x");
        serial_print_hex64(entry->len);

        serial_print(" type=");
        serial_print_dec(entry->type);

        serial_print(" ");
        serial_print(region_type_name(entry->type));
        serial_print("\n");

        cur += entry->size + sizeof(entry->size);
    }
}
