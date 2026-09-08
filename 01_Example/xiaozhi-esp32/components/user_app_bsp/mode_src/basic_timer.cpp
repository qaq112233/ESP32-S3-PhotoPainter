#include "basic_timer.h"
#include <cmath>
#include <cJSON.h>

namespace photopainter {
uint32_t ParseBasicTimerSeconds(const char* contents, size_t length) {
    if (!contents || !length || length > kMaxBasicConfigSize) return kDefaultBasicTimerSeconds;
    const char* end = nullptr;
    cJSON* root = cJSON_ParseWithLengthOpts(contents, length, &end, false);
    if (!root) return kDefaultBasicTimerSeconds;
    const cJSON* timer = cJSON_GetObjectItemCaseSensitive(root, "timer");
    uint32_t result = kDefaultBasicTimerSeconds;
    while (end < contents + length && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) ++end;
    if (end == contents + length && cJSON_IsNumber(timer)) {
        const double value = timer->valuedouble;
        if (std::isfinite(value) && value >= 1.0 && value <= UINT32_MAX && std::floor(value) == value)
            result = static_cast<uint32_t>(value);
    }
    cJSON_Delete(root);
    return result;
}
}
