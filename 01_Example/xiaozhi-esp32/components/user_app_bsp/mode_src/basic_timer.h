#pragma once
#include <cstddef>
#include <cstdint>

namespace photopainter {
constexpr uint32_t kDefaultBasicTimerSeconds = 13U * 60U;
constexpr size_t kMaxBasicConfigSize = 8U * 1024U;
uint32_t ParseBasicTimerSeconds(const char* contents, size_t length);
}
