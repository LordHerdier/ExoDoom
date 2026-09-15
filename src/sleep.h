#pragma once
#include <stdint.h>

void kernel_sleep_ms(uint32_t ms);
void kernel_sleep_until_ms(uint32_t deadline_ms);
