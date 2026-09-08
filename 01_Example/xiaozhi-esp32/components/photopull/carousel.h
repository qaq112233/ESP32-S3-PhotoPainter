#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace photopull {
struct Slide { std::string id, sha, path; };
struct DisplayIntent {
    bool available = false;
    bool clear = false;
    bool physical = true;
    Slide slide;
};
// All times are monotonic milliseconds. Only the management task owns this state.
class Carousel {
public:
    void update(std::vector<Slide> slides, uint32_t interval_sec, int64_t now);
    DisplayIntent due(int64_t now) const;
    void displayed(const DisplayIntent& intent, int64_t completed_at);
    void manual_displayed(int64_t completed_at);
    void unavailable() { known_ = false; immediate_ = urgent_clear_ = false; }
    bool urgent_clear() const { return urgent_clear_; }
    bool retry_ready(int64_t now) const { return now >= retry_at_; }
    void failed(int64_t now);
    void succeeded() { failures_ = 0; retry_at_ = 0; }
private:
    std::vector<Slide> slides_;
    std::string cursor_, screen_sha_;
    bool known_ = false, manual_ = false, empty_screen_ = false, immediate_ = false, urgent_clear_ = false;
    int64_t interval_ms_ = 300000, next_at_ = 0, retry_at_ = 0, last_completed_at_ = 0;
    unsigned failures_ = 0;
};
}
