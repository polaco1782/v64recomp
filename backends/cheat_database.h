#pragma once
#include "cheats.h"
#include <stdatomic.h>

typedef struct {
    uint32_t address;
    uint16_t value, mask;
} CheatLine;

typedef struct {
    uint16_t value;
    const char *name;
} CheatOption;

typedef struct {
    const char *name, *source;
    char *note;
    char problem[192];
    CheatLine *lines;
    CheatOption *options;
    RecompCheatCode *program;
    size_t line_count, option_count, option;
    bool enabled;
} Cheat;

typedef struct {
    Cheat *cheats;
    size_t count;
    char status[768];
    /* Names and option labels point into the two parsed source buffers. */
    char *sources[2];
} CheatCatalog;

void cheat_catalog_load(CheatCatalog *, const char *key, const char *directory, bool refresh,
                        const atomic_bool *cancel);
void cheat_catalog_destroy(CheatCatalog *);
void cheat_compile(Cheat *, const Runtime *);
