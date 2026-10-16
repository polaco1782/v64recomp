#pragma once

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <map>

namespace RecompTextures {
inline constexpr unsigned ScanIntervalSeconds = 60;
struct ScanConfig {
    std::filesystem::path source, raw, replacements;
    std::string romTitle;
};

// The scanner owns only files. RT64 state and GPU reloads remain with the host.
class Scanner {
    ScanConfig config;
    std::thread worker;
    std::mutex mutex;
    std::map<std::string, std::string> inputStamps, outputStamps;
    std::condition_variable changed;
    bool stopping = false;
    std::atomic<bool> reloadPending{false};
    void run();

public:
    explicit Scanner(ScanConfig config);
    ~Scanner();
    void start();
    void stop();
    bool takeReload() {
        return reloadPending.exchange(false);
    }
    // Also used by the native test runner with archived RT64 captures.
    size_t scanOnce();
};
}
