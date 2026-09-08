#pragma once
#include "FreeRTOS.h"
using TaskHandle_t = void*;
inline uint64_t host_now_us = 0;
inline void (*host_delay_hook)(TickType_t) = nullptr;
inline TickType_t xTaskGetTickCount() { return host_now_us / 1000; }
inline void vTaskDelay(TickType_t n) {
    host_now_us += uint64_t(n) * 1000;
    if (host_delay_hook) host_delay_hook(n);
}
inline void vTaskDelete(void*) {}
inline unsigned uxTaskGetStackHighWaterMark(void*) { return 10000; }
inline int xTaskCreate(void (*)(void*), const char*, unsigned, void*, unsigned, void**) { return pdFALSE; }
