#pragma once
#include <cstdint>
using esp_event_base_t = int;
using esp_event_handler_instance_t = void*;
constexpr int WIFI_EVENT = 1, IP_EVENT = 2, ESP_EVENT_ANY_ID = -1;
inline int esp_event_handler_instance_register(int, int, void (*)(void*, int, int32_t, void*), void*, void**) { return 0; }
inline int esp_event_handler_instance_unregister(int, int, void*) { return 0; }
