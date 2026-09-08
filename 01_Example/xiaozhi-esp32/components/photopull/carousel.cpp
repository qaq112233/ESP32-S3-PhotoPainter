#include "carousel.h"
#include <algorithm>
#include <utility>

namespace photopull {
void Carousel::update(std::vector<Slide> slides, uint32_t interval_sec, int64_t now) {
    bool first = !known_;
    bool was_empty = slides_.empty();
    slides_ = std::move(slides);
    bool interval_changed = interval_ms_ != static_cast<int64_t>(interval_sec) * 1000;
    interval_ms_ = static_cast<int64_t>(interval_sec) * 1000;
    if (!first && interval_changed && !manual_) next_at_ = last_completed_at_ + interval_ms_;
    known_ = true;
    if (slides_.empty()) {
        if (first || !was_empty) { urgent_clear_ = true; immediate_ = true; }
        return;
    }
    urgent_clear_ = false;
    if (manual_) return;
    auto current = std::find_if(slides_.begin(), slides_.end(), [&](const Slide& p) { return p.id == cursor_; });
    if (first || was_empty || current == slides_.end() || current->sha != screen_sha_) {
        immediate_ = true;
        next_at_ = now;
    }
}
DisplayIntent Carousel::due(int64_t now) const {
    DisplayIntent intent;
    if (!known_ || !retry_ready(now) || (!immediate_ && now < next_at_)) return intent;
    if (slides_.empty()) {
        if (empty_screen_ && !manual_ && !urgent_clear_) return intent;
        intent.available = intent.clear = true;
        intent.physical = !empty_screen_ || manual_;
        return intent;
    }
    auto current = std::find_if(slides_.begin(), slides_.end(), [&](const Slide& p) { return p.id == cursor_; });
    if (!immediate_ && !manual_ && slides_.size() == 1 && current != slides_.end() && current->sha == screen_sha_) return intent;
    auto selected = slides_.begin();
    if (current != slides_.end()) {
        selected = current;
        if (!immediate_) {
            ++selected;
            if (selected == slides_.end()) selected = slides_.begin();
        }
    }
    intent.available = true;
    intent.slide = *selected;
    intent.physical = manual_ || empty_screen_ || screen_sha_ != selected->sha;
    return intent;
}
void Carousel::displayed(const DisplayIntent& intent, int64_t completed_at) {
    if (!intent.available) return;
    if (!intent.clear) { cursor_ = intent.slide.id; screen_sha_ = intent.slide.sha; }
    else { screen_sha_.clear(); cursor_.clear(); }
    empty_screen_ = intent.clear;
    manual_ = false;
    immediate_ = urgent_clear_ = false;
    next_at_ = completed_at + interval_ms_;
    last_completed_at_ = completed_at;
    succeeded();
}
void Carousel::manual_displayed(int64_t completed_at) {
    manual_ = true;
    empty_screen_ = false;
    immediate_ = false;
    next_at_ = completed_at + interval_ms_;
    last_completed_at_ = completed_at;
    succeeded();
}
void Carousel::failed(int64_t now) {
    static const int64_t delays[] = {30000, 60000, 300000};
    retry_at_ = now + delays[std::min(failures_, 2u)];
    if (failures_ < 2) ++failures_;
}
}
