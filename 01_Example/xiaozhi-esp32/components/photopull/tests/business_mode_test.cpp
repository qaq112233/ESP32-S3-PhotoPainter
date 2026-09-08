#include "business_mode.h"
#include <cassert>
using photopull::ChooseBusinessMode;
int main() {
    auto m = ChooseBusinessMode(false, 0, true, false, true);
    assert(m.saved_value == 2 && m.boot_value == 2 && m.persist);
    m = ChooseBusinessMode(false, 0, false, false, true);
    assert(m.saved_value == 1 && m.boot_value == 1 && m.persist);
    m = ChooseBusinessMode(true, 1, true, false, true);
    assert(m.saved_value == 1 && m.boot_value == 1 && !m.persist);
    m = ChooseBusinessMode(true, 2, false, false, true);
    assert(m.saved_value == 2 && m.boot_value == 2 && !m.persist);
    m = ChooseBusinessMode(true, 3, false, false, true);
    assert(m.saved_value == 2 && m.boot_value == 2 && m.persist);
    m = ChooseBusinessMode(true, 3, true, true, true);
    assert(m.saved_value == 3 && m.boot_value == 3 && !m.persist);
    m = ChooseBusinessMode(true, 1, true, false, false);
    assert(m.saved_value == 1 && m.boot_value == 2 && !m.persist);
    m = ChooseBusinessMode(true, 3, false, true, false);
    assert(m.saved_value == 3 && m.boot_value == 2 && !m.persist);
    m = ChooseBusinessMode(true, 4, true, false, true);
    assert(m.boot_value == 4 && !m.persist);
}
