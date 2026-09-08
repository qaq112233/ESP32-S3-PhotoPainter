#pragma once
#include <cstdint>
#include "esp_err.h"
using gpio_num_t = int;
struct gpio_config_t { int intr_type, mode; uint64_t pin_bit_mask; int pull_down_en, pull_up_en; };
constexpr int GPIO_INTR_DISABLE = 0, GPIO_MODE_OUTPUT = 1, GPIO_MODE_INPUT = 2,
    GPIO_PULLDOWN_DISABLE = 0, GPIO_PULLUP_ENABLE = 1;
inline int gpio_config(gpio_config_t*) { return 0; }
inline void gpio_set_level(int, int) {}
inline int gpio_get_level(int) { return 1; }
