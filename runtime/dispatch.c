#include "recomp.h"

void recomp_call_generated(Runtime *rt, MipsContext *ctx, const RecompFunctionEntry *entry) {
#ifndef RECOMP_DEBUG
    /* Canonical mapping can redirect even a fixed guest address. Keep its
     * selection and prepare-call ordering identical to dynamic dispatch. */
    if (!rt->canonical_function_address) {
        RecompFunction target = entry->function;
        recomp_prepare_direct_call(rt, entry->address, entry->section);
        target(rt, ctx);
        return;
    }
#endif
    /* Debug dispatch resolves the current registry entry. */
    recomp_lookup(rt, entry->address)(rt, ctx);
}
