#pragma once
#include <cstdint>
namespace photopull {
struct BusinessMode {
    uint8_t saved_value;
    uint8_t boot_value;
    bool persist;
};
inline BusinessMode ChooseBusinessMode(bool has_saved, uint8_t saved, bool enabled_config,
                                      bool full_build, bool sd_available) {
    uint8_t selected = has_saved ? saved : (enabled_config ? 2 : 1);
    bool persist = !has_saved;
    if (!full_build && selected == 3) { selected = 2; persist = true; }
    uint8_t runtime = selected >= 1 && selected <= 4 ? selected : 1;
    if (!sd_available) runtime = 2;
    return {selected, runtime, persist};
}
}
