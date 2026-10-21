#include "cheat_database.h"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <regex>
#include <set>
#include <signal.h>
#include <spawn.h>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char **environ;
namespace RecompCheats {
namespace {
constexpr size_t MaxBytes = 8 * 1024 * 1024;
constexpr size_t MaxCheats = 4096;
struct Source { const char *key, *file; };
// Project64 now publishes individual files. Keys are cartridge header CRC1,
// CRC2 and country, and the file's own section must also match before import.
constexpr Source Project64Files[] = {
    {"30C7AC50-7704072D-C:45", "Conker's Bad Fur Day (U).cht"},
    {"04DAF07F-0D18E688-C:45", "Duke Nukem - ZERO HOUR (U).cht"},
    {"A273AB56-DA33DB9A-C:45", "Duke Nukem 64 (U).cht"},
    {"DCBC50D1-09FD1AA3-C:45", "GoldenEye 007 (U).cht"},
    {"9CAB6AEA-87C61C00-C:45", "Hexen (U).cht"},
    {"2857674D-CC4337DA-C:45", "Nightmare Creatures (U).cht"},
    {"693BA2AE-B7F14E9F-C:45", "The Legend of Zelda - Ocarina of Time (U) (V1.2).cht"},
    {"41F2B98F-B458B466-C:45", "Perfect Dark (U) (V1.1).cht"},
    {"9F5BF79C-D2FE08A0-C:45", "Quake 64 (U).cht"},
    {"BDA8F143-B1AF2D62-C:45", "Quake II (U).cht"},
    {"F3C5BF9B-160F33E2-C:45", "Rayman 2 - The Great Escape (U) (M5).cht"},
    {"3A4760B5-2D74D410-C:45", "Shadow Man (U).cht"},
    {"635A2BFF-8B022326-C:45", "Super Mario 64 (U).cht"},
};
std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
std::string upper(std::string s) {
    for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
bool hex(const std::string &s, unsigned &value) {
    if (s.empty() || s.size() > 8) return false;
    value = 0;
    for (unsigned char c : s) {
        if (!std::isxdigit(c)) return false;
        value = value * 16 + (c <= '9' ? c - '0' : std::toupper(c) - 'A' + 10);
    }
    return true;
}
std::string encode(const std::string &s) {
    std::string result;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') result += c;
        else { char escaped[4]; std::snprintf(escaped, sizeof(escaped), "%%%02X", c); result += escaped; }
    }
    return result;
}
std::string read(const std::filesystem::path &path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    auto length = f.tellg();
    if (length <= 0 || length > static_cast<std::streamoff>(MaxBytes)) return {};
    std::string text(static_cast<size_t>(length), '\0');
    f.seekg(0);
    if (!f.read(text.data(), text.size())) return {};
    return text;
}
// Run curl directly, with no shell or curlrc. TLS verification remains enabled.
// The child has both curl timeouts and an independent size/cancellation bound.
std::string download(const std::string &url, const std::filesystem::path &destination,
                     const std::atomic<bool> &cancel) {
    std::string temporary = destination.string() + ".XXXXXX";
    int fd = mkstemp(temporary.data());
    if (fd < 0) throw std::runtime_error("Cannot create cheat download cache");
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fd, STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    const char *arguments[] = {"curl", "--disable", "--fail", "--location", "--silent",
        "--proto", "=https", "--proto-redir", "=https", "--connect-timeout", "8", "--max-time", "25",
        "--max-filesize", "8388608", "--user-agent", "v64recomp-cheats/1", url.c_str(), nullptr};
    pid_t child = -1;
    int error = posix_spawnp(&child, "curl", &actions, nullptr, const_cast<char **>(arguments), environ);
    posix_spawn_file_actions_destroy(&actions);
    bool okay = error == 0;
    int status = 0;
    if (okay) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        for (;;) {
            pid_t done = waitpid(child, &status, WNOHANG);
            if (done == child) { okay = WIFEXITED(status) && WEXITSTATUS(status) == 0; break; }
            if (done < 0 && errno != EINTR) { okay = false; break; }
            struct stat info{};
            if (cancel || std::chrono::steady_clock::now() >= deadline || fstat(fd, &info) || info.st_size > static_cast<off_t>(MaxBytes)) {
                kill(child, SIGKILL);
                while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
                okay = false; break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    close(fd);
    std::string contents = okay ? read(temporary) : "";
    // An HTML error page with HTTP 200 must not replace a useful cached file.
    okay = okay && !contents.empty() && contents.find('\0') == std::string::npos &&
        (contents.find("crc ") != std::string::npos || (contents.front() == '[' && contents.find("Name=") != std::string::npos));
    if (okay) {
        std::error_code ec;
        std::filesystem::rename(temporary, destination, ec);
        if (ec) { std::filesystem::remove(temporary); throw std::runtime_error("Cannot save cheat download cache"); }
        return contents;
    }
    std::filesystem::remove(temporary);
    if (error == ENOENT) throw std::runtime_error("Install curl to download cheats");
    if (cancel) throw std::runtime_error("Download cancelled");
    if (error) throw std::runtime_error("Cannot start curl (error " + std::to_string(error) + ")");
    throw std::runtime_error("HTTPS download failed (check connection; curl exit " +
                             std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : error) + ")");
}
void add_option(Cheat &c, const std::string &value, const std::string &name) {
    unsigned n;
    if (value.size() <= 4 && hex(value, n) && c.options.size() < 1024) c.options.push_back({static_cast<uint16_t>(n), name});
}
void parse_line(Cheat &c, const std::string &text, bool project64) {
    std::istringstream input(text);
    std::string address, value;
    input >> address >> value;
    unsigned a;
    if (address.size() != 8 || value.size() != 4 || !hex(address, a)) {
        if (project64 && hex(address, a) && address.size() <= 4 && !value.empty()) {
            add_option(c, address, trim(text.substr(address.size())));
        } else if (!text.empty()) c.problem = "Unrecognized code or option line";
        return;
    }
    uint16_t v = 0, mask = 0;
    for (unsigned char digit : value) {
        v <<= 4; mask <<= 4;
        if (digit == '?') mask |= 15;
        else {
            unsigned n;
            if (!hex(std::string(1, digit), n)) { c.problem = "Invalid code value"; return; }
            v |= n;
        }
    }
    if (c.lines.size() >= 4096) { c.problem = "Too many code lines"; return; }
    c.lines.push_back({a, v, mask});
    std::string rest;
    std::getline(input, rest);
    rest = trim(rest);
    if (!rest.empty()) {
        // Mupen64Plus: ???? 0001:"Choice one",0002:"Choice two".
        static const std::regex option(R"opt(([0-9A-Fa-f]{1,4}):"([^"]*)")opt");
        size_t consumed = 0;
        for (auto i = std::sregex_iterator(rest.begin(), rest.end(), option); i != std::sregex_iterator(); ++i) {
            std::string gap = trim(rest.substr(consumed, i->position() - consumed));
            if (!gap.empty() && gap != ",") c.problem = "Unrecognized option syntax";
            add_option(c, (*i)[1], (*i)[2]); consumed = i->position() + i->length();
        }
        if (!trim(rest.substr(consumed)).empty()) c.problem = "Unrecognized option syntax";
    }
}
std::vector<Cheat> parse(const std::string &text, const std::string &key, const std::string &source, bool project64) {
    std::vector<Cheat> result;
    bool matched = false;
    Cheat *current = nullptr;
    std::istringstream stream(text);
    for (std::string raw; std::getline(stream, raw);) {
        std::string line = trim(raw);
        if (line.empty() || line.rfind("//", 0) == 0 || line.front() == '#') continue;
        if (line.rfind("crc ", 0) == 0 || (project64 && line.front() == '[')) {
            std::string id = project64 ? line.substr(1, line.size() - 2) : trim(line.substr(4));
            matched = upper(id) == key; current = nullptr; continue;
        }
        if (!matched) continue;
        if ((!project64 && line.rfind("cn ", 0) == 0) || (project64 && line.front() == '$')) {
            if (result.size() >= MaxCheats) break;
            result.emplace_back(); current = &result.back();
            current->name = line.substr(project64 ? 1 : 3); current->source = source;
        } else if (current) {
            if ((!project64 && line.rfind("cd ", 0) == 0) || (project64 && line.rfind("Note=", 0) == 0))
                current->note += (current->note.empty() ? "" : "\n") + line.substr(project64 ? 5 : 3);
            else if (line.rfind("gn ", 0) != 0 && line.rfind("Name=", 0) != 0) parse_line(*current, line, project64);
        }
    }
    return result;
}
}
std::string rom_key(const Runtime *rt) {
    if (!rt->rom || rt->rom_size < 64) return {};
    char key[32];
    const uint8_t *h = rt->rom;
    std::snprintf(key, sizeof(key), "%02X%02X%02X%02X-%02X%02X%02X%02X-C:%02X",
                  h[16], h[17], h[18], h[19], h[20], h[21], h[22], h[23], h[62]);
    return key;
}
void Cheat::compile(const Runtime *rt) {
    program.clear();
    if (!problem.empty()) return;
    for (const Line &line : lines) {
        if (line.mask && options.empty()) { problem = "Code requires an option value that the source did not provide"; return; }
        uint16_t selected = options.empty() ? 0 : options[option].value;
        program.push_back({line.address, static_cast<uint16_t>(line.value | (selected & line.mask))});
    }
    char reason[192];
    if (!recomp_cheats_validate(rt, program.data(), program.size(), reason, sizeof(reason))) problem = reason;
}
Catalog load_catalog(const std::string &key, const std::filesystem::path &directory,
                     bool refresh, const std::atomic<bool> &cancel) {
    Catalog catalog;
    try {
        std::filesystem::create_directories(directory);
        std::vector<std::pair<std::string, std::string>> sources = {{"Mupen64Plus",
            "https://raw.githubusercontent.com/mupen64plus/mupen64plus-core/master/data/mupencheat.txt"}};
        for (const Source &entry : Project64Files) if (entry.key == key)
            sources.push_back({"Project64", "https://raw.githubusercontent.com/project64/project64/develop/Config/Cheats/" + encode(entry.file)});
        std::set<std::string> seen;
        for (const auto &source : sources) {
            if (cancel) break;
            auto path = directory / (source.first + ".txt");
            std::string text = read(path), message = source.first + ": cached";
            if (refresh || text.empty()) {
                try { text = download(source.second, path, cancel); message = source.first + ": downloaded"; }
                catch (const std::exception &e) { message = source.first + ": " + e.what() + (text.empty() ? "" : "; using cache"); }
            }
            auto cheats = parse(text, key, source.first, source.first == "Project64");
            message += " (" + std::to_string(cheats.size()) + " cheats)";
            catalog.status += (catalog.status.empty() ? "" : "\n") + message;
            for (Cheat &cheat : cheats) {
                // Keep choices and malformed cheats distinct; deduplicate only
                // complete, identical fixed-code sequences across providers.
                std::string signature;
                bool fixed = cheat.problem.empty() && !cheat.lines.empty() && cheat.options.empty();
                for (const Line &line : cheat.lines) {
                    fixed &= !line.mask;
                    signature += std::to_string(line.address) + ":" + std::to_string(line.value) + ";";
                }
                if (fixed && !seen.insert(signature).second) continue;
                if (catalog.cheats.size() < MaxCheats) catalog.cheats.push_back(std::move(cheat));
            }
        }
        if (catalog.cheats.empty()) catalog.status += "\nNo codes found for this ROM's CRC and region. Refresh to retry.";
    } catch (const std::exception &e) { catalog.status += std::string("\n") + e.what(); }
    return catalog;
}
}
