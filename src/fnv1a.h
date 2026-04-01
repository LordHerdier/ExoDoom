#pragma once
#include <stdint.h>
#include <stddef.h>

uint32_t fnv1a32(const void *data, size_t len);
