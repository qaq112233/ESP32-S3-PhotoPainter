#pragma once
#include "FreeRTOS.h"
using QueueHandle_t = void*;
inline void* xQueueCreate(unsigned, unsigned) { return nullptr; }
inline int xQueueReceive(void*, void*, unsigned) { return pdFALSE; }
inline int xQueueSend(void*, const void*, unsigned) { return pdTRUE; }
inline void vQueueDelete(void*) {}
inline unsigned uxQueueMessagesWaiting(void*) { return 0; }
