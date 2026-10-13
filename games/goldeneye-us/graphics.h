#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void goldeneye_configure_graphics(void *interpreter, size_t ram_size, uint64_t *rdp_triangles);
#ifdef __cplusplus
}
#endif
