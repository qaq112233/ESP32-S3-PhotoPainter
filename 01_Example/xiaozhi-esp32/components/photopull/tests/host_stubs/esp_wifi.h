#pragma once
#include <cstdint>
using wifi_mode_t = int;
struct wifi_init_config_t {};
#define WIFI_INIT_CONFIG_DEFAULT() wifi_init_config_t{}
constexpr int WIFI_EVENT_STA_START = 10, WIFI_EVENT_STA_DISCONNECTED = 11,
    IP_EVENT_STA_GOT_IP = 12, WIFI_EVENT_AP_STACONNECTED = 13, WIFI_EVENT_AP_STADISCONNECTED = 14;
constexpr int WIFI_MODE_STA = 1, WIFI_MODE_AP = 2, WIFI_MODE_APSTA = 3,
    WIFI_AUTH_OPEN = 0, WIFI_AUTH_WPA2_PSK = 2, WIFI_IF_AP = 0, WIFI_IF_STA = 1, WIFI_STORAGE_RAM = 0;
struct wifi_config_t {
    struct { uint8_t ssid[32], password[64]; int channel, max_connection, authmode; } ap;
    struct { uint8_t ssid[32], password[64]; struct { int authmode; } threshold; } sta;
};
struct wifi_sta_list_t { uint8_t num = 0; };
inline unsigned host_connect_calls = 0;
inline int esp_wifi_init(wifi_init_config_t*) { return 0; }
inline int esp_wifi_deinit() { return 0; }
inline int esp_wifi_set_storage(int) { return 0; }
inline int esp_wifi_set_config(int, wifi_config_t*) { return 0; }
inline int esp_wifi_set_mode(int) { return 0; }
inline int esp_wifi_start() { return 0; }
inline int esp_wifi_stop() { return 0; }
inline int esp_wifi_connect() { ++host_connect_calls; return 0; }
inline int esp_wifi_ap_get_sta_list(wifi_sta_list_t*) { return 0; }
