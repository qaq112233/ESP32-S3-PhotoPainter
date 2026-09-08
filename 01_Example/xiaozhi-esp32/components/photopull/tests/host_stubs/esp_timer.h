#pragma once
#include "freertos/task.h"
inline int64_t esp_timer_get_time() { return host_now_us; }
