#include "scanner.h"
#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <unistd.h>
#include "json/json.hpp"
#include "stb/stb_image.h"
#include "stb/stb_image_write.h"

namespace fs = std::filesystem;
using json = nlohmann::json;
using RecompTextures::ScanConfig;
using RecompTextures::Scanner;

void check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
struct Fixture {
    fs::path root;
    ScanConfig config;
    explicit Fixture(const fs::path& hasher) {
        std::string pattern = (fs::temp_directory_path() / "recomp-texture-scan-XXXXXX").string();
        auto path = mkdtemp(pattern.data());
        if (!path)
            throw std::runtime_error("mkdtemp failed");
        root = path;
        config = {root / "source", root / "raw", root / "replacements", hasher, "GOLDENEYE"};
        fs::create_directories(config.source);
        fs::create_directories(config.raw / "ready");
    }
    ~Fixture() {
        fs::remove_all(root);
    }
    fs::path image(const std::string& name, uint8_t color = 64) {
        auto path = config.source / name;
        fs::create_directories(path.parent_path());
        uint8_t rgba[] = {color, color, color, 255};
        check(stbi_write_png(path.c_str(), 1, 1, 4, rgba, 4) != 0, "cannot create test PNG");
        return path;
    }
    fs::path rgba(const std::string& name, const std::vector<uint8_t>& pixels, int width = 1, int height = 1) {
        auto path = config.source / name;
        fs::create_directories(path.parent_path());
        check(pixels.size() == size_t(width * height * 4), "invalid test image pixels");
        check(stbi_write_png(path.c_str(), width, height, 4, pixels.data(), width * 4) != 0, "cannot create test PNG");
        return path;
    }
    std::vector<uint8_t> pixels(const std::string& hash) {
        auto path = config.replacements / (hash + ".v5.png");
        int width, height, channels;
        auto data = stbi_load(path.c_str(), &width, &height, &channels, 4);
        check(data != nullptr, "cannot decode imported pixels");
        std::vector<uint8_t> result(data, data + size_t(width * height * 4));
        stbi_image_free(data);
        return result;
    }
    void database(const std::vector<std::pair<std::string, std::string>>& entries) {
        json db{{"configuration", {{"hashVersion", 5}}}, {"textures", json::array()}};
        for (const auto& [hash, rice] : entries)
            db["textures"].push_back({{"hashes", {{"rt64", hash}, {"rice", rice}}}});
        std::ofstream(config.raw / "ready/rt64.json") << db;
    }
};

int main(int argc, char **argv) {
    // One-shot diagnostics use a caller-provided source copy and archived hash
    // database; the same native import path runs without creating a window.
    if (argc == 7 && std::string(argv[2]) == "--scan") {
        try {
            Scanner scanner({argv[3], argv[4], argv[5], argv[1], argv[6]});
            std::cout << "Imported " << scanner.scanOnce() << " native replacements.\n";
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
    if (argc != 2)
        return 2;
    fs::path hasher = fs::absolute(argv[1]);
    size_t passed = 0;
    auto test = [&](const char *name, const std::function<void()>& run) {
        try {
            run();
            passed++;
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            throw std::runtime_error(std::string(name) + ": " + error.what());
        }
    };
    try {
        test("moves original and persists receipt", [&] {
            Fixture f(hasher);
            auto image = f.image("GOLDENEYE#12345678#0#2_all.png");
            f.database({{"0000000000000001", "12345678#0#2"}});
            Scanner scanner(f.config);
            check(scanner.scanOnce() == 1, "did not import image");
            check(!fs::exists(image), "source was not moved");
            check(fs::exists(f.config.replacements / "0000000000000001.v5.png"), "hash name missing");
            check(scanner.takeReload() && !scanner.takeReload(), "reload notification is not consumed once");
            check(scanner.scanOnce() == 0 && !scanner.takeReload(), "unchanged files caused a reload");
        });
        test("palette wildcard survives source move and restart", [&] {
            Fixture f(hasher);
            auto image = f.image("GOLDENEYE#12345678#2#1#$_ciByRGBA.png");
            f.database({{"0000000000000001", "12345678#2#1#abcdef01"}});
            {
                Scanner scanner(f.config);
                check(scanner.scanOnce() == 1, "initial wildcard failed");
            }
            check(!fs::exists(image), "wildcard source was not moved");
            f.database({{"0000000000000001", "12345678#2#1#abcdef01"}, {"0000000000000002", "12345678#2#1#abcdef02"}});
            Scanner scanner(f.config);
            check(scanner.scanOnce() == 1, "moved wildcard cannot match a new palette");
            check(fs::exists(f.config.replacements / "0000000000000002.v5.png"), "new alias missing");
        });
        test("texture wildcard is matched", [&] {
            Fixture f(hasher);
            f.image("GOLDENEYE#$#2#1#abcdef01_ciByRGBA.png");
            f.database({{"0000000000000001", "12345678#2#1#abcdef01"}});
            Scanner scanner(f.config);
            check(scanner.scanOnce() == 1, "texture wildcard failed");
        });
        test("exact palette beats wildcard", [&] {
            Fixture f(hasher);
            auto wildcard = f.image("GOLDENEYE#12345678#2#1#$_ciByRGBA.png", 8);
            auto exact = f.image("GOLDENEYE#12345678#2#1#abcdef01_ciByRGBA.png", 32);
            f.database({{"0000000000000001", "12345678#2#1#abcdef01"}});
            Scanner scanner(f.config);
            check(scanner.scanOnce() == 1 && !fs::exists(exact) && fs::exists(wildcard), "wrong image selected");
        });
        test("ordinary filename beats editor alternates after moving", [&] {
            Fixture f(hasher);
            auto ordinary = f.image("GOLDENEYE#12345678#0#2_all.png", 32);
            auto backup = f.image("Copy of GOLDENEYE#12345678#0#2_all.png", 8);
            f.database({{"0000000000000001", "12345678#0#2"}});
            Scanner scanner(f.config);
            check(scanner.scanOnce() == 1 && !fs::exists(ordinary), "ordinary image was not moved");
            check(scanner.scanOnce() == 0 && fs::exists(backup), "alternate replaced the moved ordinary image");
        });
        test("identical duplicates are consumed without redundant reloads", [&] {
            Fixture f(hasher);
            auto a = f.image("A/GOLDENEYE#12345678#0#2_all.png");
            auto b = f.image("B/GOLDENEYE#12345678#0#2_all.png");
            f.database({{"0000000000000001", "12345678#0#2"}});
            Scanner scanner(f.config);
            check(scanner.scanOnce() == 1, "duplicate import failed");
            scanner.takeReload();
            check(scanner.scanOnce() == 0 && !scanner.takeReload(), "identical copy triggered a reload");
            check(!fs::exists(a) && !fs::exists(b), "identical duplicates were not consumed");
        });
        test("conflicting ordinary copies stay untouched", [&] {
            Fixture f(hasher);
            auto a = f.image("A/GOLDENEYE#12345678#0#2_all.png", 8);
            auto b = f.image("B/GOLDENEYE#12345678#0#2_all.png", 32);
            f.database({{"0000000000000001", "12345678#0#2"}});
            Scanner scanner(f.config);
            bool rejected = false;
            try {
                scanner.scanOnce();
            } catch (const std::runtime_error&) {
                rejected = true;
            }
            check(rejected && fs::exists(a) && fs::exists(b), "conflicting images were consumed");
        });
        test("wrong ROM pack stays untouched", [&] {
            Fixture f(hasher);
            auto image = f.image("SUPER MARIO 64#12345678#0#2_all.png");
            f.database({{"0000000000000001", "12345678#0#2"}});
            Scanner scanner(f.config);
            bool rejected = false;
            try {
                scanner.scanOnce();
            } catch (const std::runtime_error&) {
                rejected = true;
            }
            check(rejected && fs::exists(image), "wrong ROM source was accepted");
        });
        test("shutdown interrupts the fifteen-second wait", [&] {
            Fixture f(hasher);
            f.image("GOLDENEYE#12345678#0#2_all.png");
            f.database({});
            Scanner scanner(f.config);
            auto start = std::chrono::steady_clock::now();
            scanner.start();
            scanner.stop();
            check(std::chrono::steady_clock::now() - start < std::chrono::seconds(1), "shutdown waited for scan timer");
        });
        test("nested source and output are rejected", [&] {
            Fixture f(hasher);
            f.config.replacements = f.config.source / "nested";
            bool rejected = false;
            try {
                Scanner scanner(f.config);
            } catch (const std::runtime_error&) {
                rejected = true;
            }
            check(rejected, "nested output accepted");
        });
        test("RGB and mask combine using the mask red channel", [&] {
            Fixture f(hasher);
            auto rgb = f.rgba("GOLDENEYE#12345678#0#2_rgb.png", {10, 20, 30, 9, 40, 50, 60, 11}, 2);
            auto alpha = f.rgba("GOLDENEYE#12345678#0#2_a.png", {0, 99, 88, 255, 128, 77, 66, 3}, 2);
            f.database({{"0000000000000001", "12345678#0#2"}});
            Scanner scanner(f.config);
            check(scanner.scanOnce() == 1, "split texture not imported");
            check(f.pixels("0000000000000001") == std::vector<uint8_t>({10, 20, 30, 0, 40, 50, 60, 128}),
                  "incorrect RGB/mask combination");
            check(!fs::exists(rgb) && !fs::exists(alpha), "split components were not moved");
        });
        test("unpaired RGB is opaque", [&] {
            Fixture f(hasher);
            f.rgba("GOLDENEYE#12345678#0#2_rgb.png", {10, 20, 30, 9});
            f.database({{"0000000000000001", "12345678#0#2"}});
            Scanner scanner(f.config);
            check(scanner.scanOnce() == 1, "RGB texture not imported");
            check(f.pixels("0000000000000001") == std::vector<uint8_t>({10, 20, 30, 255}),
                  "unpaired RGB retained an alpha channel");
        });
        test("split wildcard preserves alpha for later palette aliases", [&] {
            Fixture f(hasher);
            auto rgb = f.rgba("GOLDENEYE#12345678#2#1#$_RGB.PNG", {9, 8, 7, 255});
            auto alpha = f.rgba("GOLDENEYE#12345678#2#1#$_A.PNG", {96, 0, 0, 255});
            f.database({{"0000000000000001", "12345678#2#1#abcdef01"}});
            {
                Scanner scanner(f.config);
                check(scanner.scanOnce() == 1, "split wildcard import failed");
            }
            check(!fs::exists(rgb) && !fs::exists(alpha), "split wildcard sources were not consumed");
            f.database({{"0000000000000002", "12345678#2#1#abcdef02"}});
            Scanner scanner(f.config);
            check(scanner.scanOnce() == 1, "new palette alias failed after moving split sources");
            check(f.pixels("0000000000000002") == std::vector<uint8_t>({9, 8, 7, 96}),
                  "palette alias lost merged alpha");
        });
        test("mismatched split dimensions preserve original files", [&] {
            Fixture f(hasher);
            auto rgb = f.image("GOLDENEYE#12345678#0#2_rgb.png");
            auto alpha = f.rgba("GOLDENEYE#12345678#0#2_a.png", {0, 0, 0, 255, 128, 128, 128, 255}, 2);
            f.database({{"0000000000000001", "12345678#0#2"}});
            Scanner scanner(f.config);
            bool rejected = false;
            try {
                scanner.scanOnce();
            } catch (const std::runtime_error&) {
                rejected = true;
            }
            check(rejected && fs::exists(rgb) && fs::exists(alpha), "invalid split texture was consumed");
        });
        test("complete split pair takes precedence over combined file", [&] {
            Fixture f(hasher);
            f.rgba("GOLDENEYE#12345678#0#2_rgb.png", {10, 20, 30, 255});
            f.rgba("GOLDENEYE#12345678#0#2_a.png", {128, 128, 128, 255});
            auto combined = f.image("GOLDENEYE#12345678#0#2_all.png", 64);
            f.database({{"0000000000000001", "12345678#0#2"}});
            {
                Scanner scanner(f.config);
                check(scanner.scanOnce() == 1, "pair did not import");
            }
            Scanner scanner(f.config);
            check(scanner.scanOnce() == 0 && fs::exists(combined), "combined fallback replaced moved split texture");
            check(f.pixels("0000000000000001") == std::vector<uint8_t>({10, 20, 30, 128}), "wrong preferred image");
        });
        test("legacy receipts protect combined alpha from leftover RGB", [&] {
            Fixture f(hasher);
            f.rgba("GOLDENEYE#12345678#0#2_all.png", {10, 20, 30, 128});
            f.database({{"0000000000000001", "12345678#0#2"}});
            {
                Scanner scanner(f.config);
                check(scanner.scanOnce() == 1, "combined import failed");
            }
            auto metadata = f.config.replacements / ".texture-scan.json";
            json receipt;
            {
                std::ifstream stream(metadata);
                stream >> receipt;
            }
            receipt.erase("formats");
            std::ofstream(metadata) << receipt;
            auto rgb = f.image("GOLDENEYE#12345678#0#2_rgb.png", 8);
            Scanner scanner(f.config);
            check(scanner.scanOnce() == 0 && fs::exists(rgb), "leftover RGB replaced a legacy combined texture");
            check(f.pixels("0000000000000001")[3] == 128, "legacy transparency lost");
        });
        test("combined palette filename alias is supported", [&] {
            Fixture f(hasher);
            f.image("GOLDENEYE#12345678#2#0#abcdef01_allciByRGBA.png");
            f.database({{"0000000000000001", "12345678#2#0#abcdef01"}});
            Scanner scanner(f.config);
            check(scanner.scanOnce() == 1, "allciByRGBA image ignored");
        });
        std::cout << passed << " native texture scanner tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
