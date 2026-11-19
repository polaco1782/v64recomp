#pragma once
#include "xxHash/xxh3.h"
#include "common/rt64_tmem_hasher.h"
#include <string>

namespace RecompTexturePaths {
inline constexpr char RiceOverrides[] = "v64recomp/overrides/rice";
inline constexpr char Imports[] = "v64recomp/imports";

inline std::string rt64Prefix() {
    return "v64recomp/overrides/rt64-v" + std::to_string(RT64::TMEMHasher::CurrentHashVersion) + "/";
}

inline bool isRT64Override(const std::string& name) {
    const auto prefix = rt64Prefix();
    return name.size() == prefix.size() + 20 && name.compare(0, prefix.size(), prefix) == 0 &&
           name.compare(name.size() - 4, 4, ".png") == 0 &&
           name.find_first_not_of("0123456789abcdef", prefix.size()) == name.size() - 4;
}
}
