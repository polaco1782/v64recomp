#include "run.h"
int main(int argc, char **argv) {
    RecompGameOptions options;
    int parsed = recomp_game_options(argc, argv, &options);
    return parsed ? (parsed < 0 ? 2 : 0) : zero_hour_run(&options, NULL);
}
