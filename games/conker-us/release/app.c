#include "conker.h"

int main(int argc, char **argv) {
    RecompGameOptions options;
    int parsed = recomp_game_options(argc, argv, &options);
    if (parsed)
        return parsed < 0 ? 2 : 0;
    Conker game;
    conker_start(&game, &options);
    for (uint32_t frame = 0; !options.frames || frame < options.frames; frame++) {
        RecompPad pad = {.connected = true};
        if (!recomp_renderer_input(game.renderer, &pad))
            break;
        conker_step(&game, &pad);
    }
    conker_stop(&game);
    return 0;
}
