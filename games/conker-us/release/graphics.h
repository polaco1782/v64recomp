#ifndef CONKER_GRAPHICS_H
#define CONKER_GRAPHICS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
bool conker_bind_microcode(void *interpreter, size_t ram_size, uint32_t code, uint32_t data);
#ifdef __cplusplus
}
#endif
#endif
