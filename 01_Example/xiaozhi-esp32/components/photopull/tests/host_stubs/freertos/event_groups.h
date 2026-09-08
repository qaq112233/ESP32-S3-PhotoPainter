#pragma once
#include "task.h"
using EventBits_t = uint32_t;
struct HostEventGroup { EventBits_t bits = 0; };
using EventGroupHandle_t = HostEventGroup*;
struct HostTaskDone {};
inline bool host_stop_on_empty_events = false;
inline EventGroupHandle_t xEventGroupCreate() { return new HostEventGroup; }
inline EventBits_t xEventGroupSetBits(EventGroupHandle_t g, EventBits_t bits) { return g->bits |= bits; }
inline EventBits_t xEventGroupClearBits(EventGroupHandle_t g, EventBits_t bits) {
    const auto old = g->bits; g->bits &= ~bits; return old;
}
inline EventBits_t xEventGroupWaitBits(EventGroupHandle_t g, EventBits_t wanted,
                                      bool clear, bool, TickType_t wait) {
    const auto bits = g->bits & wanted;
    if (!bits && host_stop_on_empty_events) throw HostTaskDone{};
    if (clear) g->bits &= ~wanted;
    if (!bits) vTaskDelay(wait);
    return bits;
}
