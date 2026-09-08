#pragma once
template<class... T> inline void HostLog(T&&...) {}
#define ESP_LOGE(...) HostLog(__VA_ARGS__)
#define ESP_LOGW(...) HostLog(__VA_ARGS__)
#define ESP_LOGI(...) HostLog(__VA_ARGS__)
