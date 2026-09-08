#pragma once
#include <cstdint>
#include <functional>
inline std::function<uint64_t()> host_free_space;
inline int esp_vfs_fat_info(const char*, uint64_t* total, uint64_t* free) {
    *total = 1000000000;
    *free = host_free_space ? host_free_space() : *total;
    return 0;
}
