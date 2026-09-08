#pragma once
#include "task.h"
using SemaphoreHandle_t = void*;
inline void* xSemaphoreCreateMutex() { return reinterpret_cast<void*>(1); }
inline int xSemaphoreTake(void*, unsigned) { return pdTRUE; }
inline int xSemaphoreGive(void*) { return pdTRUE; }
inline void vSemaphoreDelete(void*) {}
