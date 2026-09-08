#pragma once
struct esp_netif_t {};
inline esp_netif_t host_sta_netif, host_ap_netif;
inline int esp_netif_init() { return 0; }
inline esp_netif_t* esp_netif_create_default_wifi_sta() { return &host_sta_netif; }
inline esp_netif_t* esp_netif_create_default_wifi_ap() { return &host_ap_netif; }
