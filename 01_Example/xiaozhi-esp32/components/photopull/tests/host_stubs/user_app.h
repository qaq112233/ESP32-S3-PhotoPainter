#pragma once
#include <atomic>
#include "freertos/event_groups.h"
#include "display_bsp.h"
extern ePaperPort ePaperDisplay;
extern std::atomic<uint8_t> Green_led_arg;
extern SemaphoreHandle_t epaper_gui_semapHandle;
extern EventGroupHandle_t Green_led_Mode_queue;
