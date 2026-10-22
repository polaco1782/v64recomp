#include "hexen.h"
int main(int argc, char **argv) {
    RecompGameOptions options;
    int parsed = recomp_game_options(argc, argv, &options);
    if (parsed)
        return parsed < 0 ? 2 : 0;
    Hexen game;
    hexen_init(&game, &options, NULL);
    uint64_t deadline = recomp_game_monotonic();
    for (uint32_t frame = 0; !options.frames || frame < options.frames; frame++) {
        RecompPad pad = {.connected = true};
        if (!recomp_renderer_input(game.renderer, &pad))
            break;
        hexen_step(&game, &pad);
        recomp_game_require(recomp_renderer_present(game.renderer), "Hexen presentation failed", 0);
        deadline += 16666667;
        hexen_pace(&game, deadline);
    }
    hexen_destroy(&game);
    return 0;
}
