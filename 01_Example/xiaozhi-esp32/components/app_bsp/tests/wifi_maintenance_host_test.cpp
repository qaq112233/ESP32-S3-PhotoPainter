// Execute the production callbacks and maintenance task with coalesced bits.
#include <cassert>
#include <cstdio>
#include "../server_app.cpp"

bool photo_connected = false;
unsigned state_notifications = 0;
void photopull_network_changed(bool connected) { photo_connected = connected; ++state_notifications; }

void RunEvents(bool last_is_connected) {
    HostEventGroup events;
    s_wifi_events = &events;
    s_sta_netif = &host_sta_netif;
    s_wifi_started = true; s_auto_fallback_ap = false; s_ap_active = false;
    s_sta_connected = false; photo_connected = false; state_notifications = 0;
    s_retry_delay_ms = 40000; host_connect_calls = 0; host_now_us = 1000000;
    host_stop_on_empty_events = true;
    auto connected = [] { sta_wifi_event_callback(nullptr, IP_EVENT, IP_EVENT_STA_GOT_IP, nullptr); };
    auto disconnected = [] { sta_wifi_event_callback(nullptr, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, nullptr); };
    if (last_is_connected) { disconnected(); connected(); }
    else { connected(); disconnected(); }
    assert(events.bits == (WIFI_EVENT_STA_DISCONNECTED_BIT | WIFI_EVENT_STA_GOT_IP_BIT));
    try { wifi_maintenance_task(nullptr); } catch (const HostTaskDone&) {}
    assert(s_sta_connected == last_is_connected && photo_connected == last_is_connected);
    assert(state_notifications == 2); // No stale replay from the task.
    assert(host_connect_calls == 0);
    if (last_is_connected) assert(s_retry_delay_ms == WIFI_RETRY_INITIAL_MS);
    else assert(s_next_retry > xTaskGetTickCount());
    s_wifi_events = nullptr; host_stop_on_empty_events = false;
}
int main() {
    RunEvents(true);
    RunEvents(false);
    puts("coalesced Wi-Fi events preserve the latest connection state");
}
