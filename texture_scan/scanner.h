#pragma once

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace RecompTextures {
struct ScanConfig {
    std::filesystem::path source, raw, replacements, hasher;
    std::string romTitle;
};

// The scanner owns only files. RT64 state and GPU reloads remain with the host.
class Scanner {
    ScanConfig config;
    std::thread worker;
    std::mutex mutex;
    std::mutex captures;
    std::condition_variable changed;
    bool stopping = false;
    std::atomic<bool> reloadPending{false};
    void run();
public:
    explicit Scanner(ScanConfig config);
    ~Scanner();
    void start();
    void stop();
    bool takeReload() { return reloadPending.exchange(false); }
    std::unique_lock<std::mutex> guardCaptures() { return std::unique_lock<std::mutex>(captures); }
    // Also used by the native test runner with archived RT64 captures.
    size_t scanOnce();
};
}
