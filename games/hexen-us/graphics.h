#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
bool hexen_bind_microcode(void *interpreter, size_t ram_size, uint32_t code, uint32_t data);
#ifdef __cplusplus
}
#endif
