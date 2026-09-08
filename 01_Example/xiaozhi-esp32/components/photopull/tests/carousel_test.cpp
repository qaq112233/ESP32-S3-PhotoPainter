#include "carousel.h"
#include <cassert>
#include <iostream>
using namespace photopull;
int main() {
    Carousel c;
    Slide a{"a", "sha-a", "a.bmp"}, b{"b", "sha-b", "b.bmp"};
    assert(!c.due(0).available);
    c.update({a, b}, 60, 0);
    auto d = c.due(0); assert(d.available && d.slide.id == "a");
    c.displayed(d, 5000);
    assert(!c.due(64999).available);
    d = c.due(65000); assert(d.slide.id == "b"); c.displayed(d, 70000);
    c.update({b, a}, 60, 80000); assert(!c.due(80000).available);
    c.manual_displayed(90000);
    c.update({a, b}, 60, 100000); assert(!c.due(100000).available);
    d = c.due(150000); assert(d.slide.id == "a"); c.displayed(d, 160000);
    c.update({a}, 60, 170000); assert(!c.due(9999999).available);
    c.manual_displayed(180000); d = c.due(240000); assert(d.available && d.physical); c.displayed(d, 240000);
    Slide replacement = a; replacement.sha = "new";
    c.update({replacement}, 60, 250000); d = c.due(250000); assert(d.available && d.physical);
    c.failed(250000); assert(!c.due(279999).available); assert(c.due(280000).available);
    c.failed(280000); assert(!c.due(339999).available); assert(c.due(340000).available);
    c.failed(340000); assert(!c.due(639999).available); assert(c.due(640000).available);
    c.displayed(c.due(640000), 650000);
    c.update({}, 60, 660000); assert(c.urgent_clear()); d = c.due(660000); assert(d.clear); c.displayed(d, 670000);
    c.update({}, 60, 680000); assert(!c.due(9999999).available);
    c.manual_displayed(690000); assert(!c.due(749999).available); assert(c.due(750000).clear);
    c.displayed(c.due(750000), 750000);
    Slide same = a; same.id = "same";
    c.update({a, same}, 60, 760000); c.displayed(c.due(760000), 760000);
    d = c.due(820000); assert(d.available && !d.physical && d.slide.id == "same");
    c.displayed(d, 820000); assert(!c.due(820001).available);
    c.update({a, same}, 120, 821000);
    assert(!c.due(939999).available);
    assert(c.due(940000).available);
    c.unavailable(); assert(!c.due(9999999).available);
    c.update({a}, 60, 10000000); assert(c.due(10000000).available);
    std::cout << "carousel scenarios passed\n";
}
