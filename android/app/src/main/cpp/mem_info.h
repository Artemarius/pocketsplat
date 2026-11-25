#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace pocketsplat {

// The system's MemAvailable (/proc/meminfo) in bytes, 0 when unreadable. On a phone GPU memory is
// system RAM, so this bounds both opening a scene and importing one (ADR-0018): Adreno drivers may
// lack VK_EXT_memory_budget, and VMA's fallback (80 % of the heap) is far above what Android gives
// an app (S25U: 8.9 GB against 6.0 GB MemAvailable).
inline std::uint64_t mem_available_bytes() {
    std::FILE* f = std::fopen("/proc/meminfo", "r");
    if (f == nullptr) return 0;
    char key[64];
    unsigned long long kb = 0, found = 0;
    while (std::fscanf(f, "%63s %llu kB", key, &kb) == 2) {  // MemAvailable is the third line
        if (std::strcmp(key, "MemAvailable:") == 0) {
            found = kb;
            break;
        }
    }
    std::fclose(f);
    return found * 1024ull;
}

}  // namespace pocketsplat
