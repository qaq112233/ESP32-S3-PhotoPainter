#pragma once
#include <cstdint>
using TickType_t = uint32_t;
using BaseType_t = int;
using UBaseType_t = unsigned;
constexpr int pdTRUE = 1, pdFALSE = 0, pdPASS = 1;
constexpr unsigned portMAX_DELAY = ~0u;
#define pdMS_TO_TICKS(x) (x)
constexpr unsigned BIT0 = 1u, BIT1 = 2u, BIT2 = 4u, BIT3 = 8u;
