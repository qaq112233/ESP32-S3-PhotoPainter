#pragma once
inline int mdns_init() { return 0; }
inline void mdns_hostname_set(const char*) {}
inline void mdns_instance_name_set(const char*) {}
inline void mdns_service_add(const char*, const char*, const char*, int, void*, int) {}
