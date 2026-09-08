#pragma once

#include <cstddef>
#include <cstdint>
#include "esp_err.h"

// Configuration is immutable after startup. No credentials are persisted here.
void photopull_load_config(bool storage_available);
bool photopull_default_network();
bool photopull_wifi_credentials(char* ssid, size_t ssid_capacity,
                               char* password, size_t password_capacity);
bool photopull_start();
bool photopull_ready();
bool photopull_active();
bool photopull_stop(uint32_t timeout_ms);
void photopull_network_changed(bool connected);
bool photopull_request_battery();

// These bounded RPCs copy their payload; HTTP tasks never own open SD files.
// Upload success means durable save and acceptance for display, not refresh completion.
esp_err_t photopull_upload_begin(size_t bmp_size);
esp_err_t photopull_upload_write(const uint8_t* data, size_t size);
esp_err_t photopull_upload_finish();
void photopull_upload_abort();
esp_err_t photopull_web_read(const char* path, size_t offset, uint8_t* out,
                           size_t capacity, size_t* bytes_read, size_t* total_size);
