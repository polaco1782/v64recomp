#define _POSIX_C_SOURCE 200809L
#include "game.h"
#include "runtime_debug.h"
#include "game_defaults.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int recomp_debug_main(int argc, char **argv);
static void setting(const char *suffix, const char *value) {
    const char *id = RECOMP_GAME_ID;
    char key[128] = "RECOMP_";
    size_t n = strlen(key);
    while (*id && *id != '-') {
        char c = *id++;
        key[n++] = c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
    }
    snprintf(key + n, sizeof(key) - n, "_%s", suffix);
    setenv(key, value, 1);
}
int main(int argc, char **argv) {
    RECOMP_LOG("game", "%s debug diagnostics enabled; frequent events sampled (RECOMP_DEBUG_FULL=1 logs all)",
               RECOMP_GAME_ID);
    for (int i = 1; i < argc; i++)
        RECOMP_LOG("game", "argument[%d]=%s", i, argv[i]);
    /* Keep the diagnostic harness's positional interface available to tools. */
    if (argc > 1 && argv[1][0] != '-')
        return recomp_debug_main(argc, argv);
    char **filtered = calloc((size_t)argc + 1, sizeof(*filtered));
    if (!filtered)
        return 2;
    int count = 1;
    filtered[0] = argv[0];
    bool cheats = false;
    const char *mode = NULL;
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (!strcmp(arg, "--help")) {
            puts("Debug: --trace-calls --input FILE --wav FILE --audio-trace FILE --capture-every N --cheat-menu "
                 "--mode MODE");
            puts("Input files contain frame, buttons, stick_x, stick_y columns. SM64 uses --mode "
                 "live/idle/move/jump/interior.");
        }
        if (!strcmp(arg, "--trace-calls")) {
            setenv("RECOMP_TRACE_CALLS", "1", 1);
            continue;
        }
        if (!strcmp(arg, "--cheat-menu")) {
            cheats = true;
            continue;
        }
        const char *suffix = !strcmp(arg, "--input")           ? "INPUT"
                             : !strcmp(arg, "--wav")           ? "WAV"
                             : !strcmp(arg, "--audio-trace")   ? "AUDIO_TRACE"
                             : !strcmp(arg, "--capture-every") ? "CAPTURE_EVERY"
                                                               : NULL;
        if (suffix || !strcmp(arg, "--mode")) {
            if (++i >= argc) {
                fprintf(stderr, "missing value for %s\n", arg);
                free(filtered);
                return 2;
            }
            if (suffix)
                setting(suffix, argv[i]);
            else
                mode = argv[i];
            continue;
        }
        filtered[count++] = argv[i];
    }
    RecompGameOptions o;
    int parsed = recomp_game_options(count, filtered, &o);
    free(filtered);
    if (parsed)
        return parsed < 0 ? 2 : 0;
    RECOMP_LOG("game", "ROM=%s data=%s save=%s frames=%u mute=%s output=%s", o.rom, o.data, o.save, o.frames,
               o.mute ? "yes" : "no", RECOMP_DEBUG_OUTPUT);
    bool sm64 = !strcmp(RECOMP_GAME_ID, "sm64-us"), goldeneye = !strcmp(RECOMP_GAME_ID, "goldeneye-us");
    if (cheats && !sm64 && !goldeneye) {
        fputs("--cheat-menu is available for SM64 and GoldenEye\n", stderr);
        return 2;
    }
    if (mode && !sm64) {
        fputs("--mode is available for SM64\n", stderr);
        return 2;
    }
    if (o.mute)
        setenv("RECOMP_AUDIO_MUTE", "1", 1);
    setting("SAVE", o.save);
    if (goldeneye)
        setenv("RECOMP_GOLDENEYE_SAVE", o.save, 1);
    if (sm64)
        setenv("RECOMP_CAPTURE", "1", 1);
    char frames[32];
    snprintf(frames, sizeof(frames), "%u", o.frames);
    char report[4096], data[4096];
    snprintf(report, sizeof(report), "%s/boot-report.json", RECOMP_DEBUG_OUTPUT);
    snprintf(data, sizeof(data), "%s/startup-data.bin", RECOMP_DEBUG_OUTPUT);
    /* CMake creates the debug output directory when configuring this target. */
    char *args[8] = {argv[0]};
    int n = 1;
    args[n++] = (char *)o.rom;
    if (sm64) {
        args[n++] = RECOMP_DEBUG_OUTPUT;
        args[n++] = (char *)o.save;
        args[n++] = frames;
        args[n++] = (char *)(mode ? mode : o.frames ? "idle" : "live");
    } else if (goldeneye) {
        args[n++] = data;
        args[n++] = report;
        args[n++] = frames;
    } else {
        args[n++] = report;
        args[n++] = frames;
        args[n++] = (char *)o.data;
    }
    if (cheats)
        args[n++] = "--cheat-menu";
    args[n] = NULL;
    int status = recomp_debug_main(n, args);
    RECOMP_LOG("game", "%s exited: status=%d", RECOMP_GAME_ID, status);
    return status;
}
