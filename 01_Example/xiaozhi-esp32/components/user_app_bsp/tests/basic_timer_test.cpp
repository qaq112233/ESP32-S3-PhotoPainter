#include "../mode_src/basic_timer.h"
#include <cassert>
#include <cstring>
#include <string>
using namespace photopainter;
uint32_t Parse(const char* text) { return ParseBasicTimerSeconds(text, std::strlen(text)); }
int main() {
    for (const char* invalid : {"{}", "[]", "{", R"({"timer":0})", R"({"timer":-1})",
         R"({"timer":0.5})", R"({"timer":1.5})", R"({"timer":4294967296})",
         R"({"timer":1e400})", R"({"timer":"60"})", R"({"timer":60}garbage)"})
        assert(Parse(invalid) == kDefaultBasicTimerSeconds);
    assert(Parse(R"({"timer":1})") == 1);
    assert(Parse("{\"timer\":60}\n ") == 60);
    assert(Parse(R"({"timer":4294967295})") == UINT32_MAX);
    assert(Parse(R"({"timer":300,"ai_model":"unused"})") == 300);
    assert(ParseBasicTimerSeconds(nullptr, 0) == kDefaultBasicTimerSeconds);
    const std::string large(kMaxBasicConfigSize + 1, ' ');
    assert(ParseBasicTimerSeconds(large.data(), large.size()) == kDefaultBasicTimerSeconds);
}
