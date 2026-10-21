#pragma once
#include "cheats.h"
#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

namespace RecompCheats {
struct Option { uint16_t value; std::string name; };
struct Line { uint32_t address; uint16_t value, mask; };
struct Cheat {
    std::string name, note, source, problem;
    std::vector<Line> lines;
    std::vector<Option> options;
    std::vector<RecompCheatCode> program;
    int option = 0;
    bool enabled = false;
    void compile(const Runtime *runtime);
};
struct Catalog {
    std::vector<Cheat> cheats;
    std::string status;
};
std::string rom_key(const Runtime *runtime);
/* Background I/O only: never reads Runtime or touches ImGui. */
Catalog load_catalog(const std::string &key, const std::filesystem::path &directory,
                     bool refresh, const std::atomic<bool> &cancel);
}
