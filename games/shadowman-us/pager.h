#ifndef SHADOWMAN_PAGER_H
#define SHADOWMAN_PAGER_H
#include "runtime.h"
/* Converts a native RAM miss into the game's original fault-service request. */
bool shadowman_resolve_address(Runtime *, uint32_t, size_t, bool, uint32_t *);
#endif
