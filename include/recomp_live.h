#ifndef RECOMP_LIVE_H
#define RECOMP_LIVE_H

#include "recomp.h"

#define RECOMP_LIVE_ABI 1u

typedef struct RecompLiveTransfer {
    uint32_t site, target, section;
} RecompLiveTransfer;
typedef void (*RecompLiveResume)(Runtime *, MipsContext *, uint32_t start_pc, uint32_t return_address,
                                 uint32_t entry_stack_pointer);

/* Export exactly one symbol named recomp_live_manifest from a repair module.
 * The module and all of its code must remain loaded while guest stacks exist. */
typedef struct RecompLiveManifest {
    uint32_t abi;
    uint32_t debug_build;
    uint32_t runtime_size, context_size, entry_size;
    uint8_t rom_sha1[20];
    const RecompFunctionEntry *functions;
    size_t function_count;
    const RecompLiveTransfer *calls;
    size_t call_count;
    const RecompLiveTransfer *jumps;
    size_t jump_count;
    RecompLiveResume resume;
} RecompLiveManifest;

#endif
