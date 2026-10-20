#include "run.h"
#include <stdlib.h>
/* The shared Debug CLI supplies this positional diagnostic interface. */
int recomp_debug_main(int argc, char **argv) {
    if (argc != 5) return 2;
    char *args[] = {argv[0], "--rom", argv[1], "--frames", argv[3], "--data", argv[4]};
    RecompGameOptions options;
    int parsed = recomp_game_options(7, args, &options);
    if (parsed) return parsed < 0 ? 2 : 0;
    options.mute = getenv("RECOMP_AUDIO_MUTE") != NULL;
    const char *save = getenv("RECOMP_RAYMAN2_SAVE");
    if (save) options.save = save;
    return rayman2_run(&options, argv[2]);
}
