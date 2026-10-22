#ifndef RECOMP_CHEATS_H
#define RECOMP_CHEATS_H
#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef struct RecompCheatCode {
    uint32_t address;
    uint16_t value;
} RecompCheatCode;

/* Owner-thread only, while guest execution is parked. Validate the entire
 * cheat before enabling it; unsupported instructions never partially execute.
 * Codes address physical RDRAM, not the game's virtual-address resolver. */
bool recomp_cheats_validate(const Runtime *, const RecompCheatCode *, size_t, char *reason, size_t reason_size);
void recomp_cheats_apply(Runtime *, const RecompCheatCode *, size_t, bool gs_button);
#ifdef __cplusplus
}
#endif
#endif
