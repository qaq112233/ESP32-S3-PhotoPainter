#pragma once
#include "freertos/event_groups.h"
constexpr unsigned GroupBit0 = 1, GroupBit1 = 2, GroupBit2 = 4, GroupBit3 = 8;
inline unsigned set_bit_button(unsigned n) { return 1u << n; }
