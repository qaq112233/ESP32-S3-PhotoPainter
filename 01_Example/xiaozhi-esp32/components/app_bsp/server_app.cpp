#include <stdio.h>
#include <string.h>
#include <atomic>

#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "server_app.h"
#include "server_protocol.h"
#include "button_bsp.h"
#include "mdns.h"
#include "service.h"

static const char *TAG = "server_bsp";

static esp_err_t send_unavailable(httpd_req_t* req, const char* message) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, message, HTTPD_RESP_USE_STRLEN);
}


/* The HTTP task and the PhotoPull manager communicate in bounded chunks. */
static constexpr size_t WEB_READ_MAX = photopainter_server::kWebReadMax;
static constexpr uint32_t WIFI_FIRST_CONNECT_TIMEOUT_MS = 20000;
static constexpr uint32_t WIFI_RETRY_INITIAL_MS = 5000;
static constexpr uint32_t WIFI_RETRY_MAX_MS = 60000;
static constexpr uint8_t BSP_ESP_WIFI_CHANNEL = 1;
static constexpr uint8_t BSP_MAX_STA_CONN = 4;

/* This AP is intentionally kept compatible with the existing maintenance UI. */
static constexpr char BSP_ESP_WIFI_SSID[] = "esp_network";
static constexpr char BSP_ESP_WIFI_PASS[] = "1234567890";

EventGroupHandle_t ServerPortGroups = NULL;

static EventGroupHandle_t s_wifi_events = NULL;
static QueueHandle_t s_ap_client_events = NULL;
static TaskHandle_t s_wifi_task = NULL;
static esp_event_handler_instance_t s_wifi_handler = NULL;
static esp_event_handler_instance_t s_ip_handler = NULL;
static esp_event_handler_instance_t s_ap_handler = NULL;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static httpd_handle_t s_http_server = NULL;
static std::atomic<bool> s_http_ready{false};
static std::atomic<bool> s_wifi_initialized{false};
static std::atomic<bool> s_wifi_started{false};
static std::atomic<bool> s_sta_connected{false};
static std::atomic<bool> s_ap_active{false};
static std::atomic<bool> s_auto_fallback_ap{false};
static uint8_t s_ap_client_count = 0;
static uint32_t s_retry_delay_ms = WIFI_RETRY_INITIAL_MS;
static TickType_t s_next_retry = 0;
static std::atomic<uint8_t> netMode{0};

const char staresp[] = "1";
const char apresp[] = "0";

enum : EventBits_t {
    WIFI_EVENT_STA_START_BIT = BIT0,
    WIFI_EVENT_STA_GOT_IP_BIT = BIT1,
    WIFI_EVENT_STA_DISCONNECTED_BIT = BIT2,
    WIFI_EVENT_MAINTENANCE_REQUEST_BIT = BIT3,
};

enum : uint8_t {
    AP_CLIENT_CONNECTED = 1,
    AP_CLIENT_DISCONNECTED = 2,
};

static bool tick_reached(TickType_t now, TickType_t target) {
    return static_cast<int32_t>(now - target) >= 0;
}

static void schedule_sta_retry(TickType_t now) {
    s_next_retry = now + pdMS_TO_TICKS(s_retry_delay_ms);
    s_retry_delay_ms = (s_retry_delay_ms >= WIFI_RETRY_MAX_MS / 2)
                           ? WIFI_RETRY_MAX_MS
                           : s_retry_delay_ms * 2;
}

static void ensure_server_groups(void) {
    if (ServerPortGroups == NULL) {
        ServerPortGroups = xEventGroupCreate();
    }
}

static void ensure_wifi_events(void) {
    if (s_wifi_events == NULL) {
        s_wifi_events = xEventGroupCreate();
    }
    if (s_ap_client_events == NULL) {
        s_ap_client_events = xQueueCreate(8, sizeof(uint8_t));
    }
}

static esp_err_t configure_ap(void) {
    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (s_ap_netif == NULL) {
            ESP_LOGE(TAG, "Failed to create the maintenance AP interface");
            return ESP_ERR_NO_MEM;
        }
    }

    wifi_config_t wifi_config = {};
    snprintf(reinterpret_cast<char *>(wifi_config.ap.ssid), sizeof(wifi_config.ap.ssid),
             "%s", BSP_ESP_WIFI_SSID);
    snprintf(reinterpret_cast<char *>(wifi_config.ap.password), sizeof(wifi_config.ap.password),
             "%s", BSP_ESP_WIFI_PASS);
    wifi_config.ap.channel = BSP_ESP_WIFI_CHANNEL;
    wifi_config.ap.max_connection = BSP_MAX_STA_CONN;
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    return esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
}

static void sta_wifi_event_callback(void *arg, esp_event_base_t event_base,
                                    int32_t event_id, void *event_data) {
    (void)arg;
    (void)event_data;
    if (s_wifi_events == NULL) {
        return;
    }
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        xEventGroupSetBits(s_wifi_events, WIFI_EVENT_STA_START_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_connected = false;
        photopull_network_changed(false);
        xEventGroupSetBits(s_wifi_events, WIFI_EVENT_STA_DISCONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_sta_connected = true;
        photopull_network_changed(true);
        xEventGroupSetBits(s_wifi_events, WIFI_EVENT_STA_GOT_IP_BIT);
    }
}

static void ap_wifi_event_callback(void *arg, esp_event_base_t event_base,
                                   int32_t event_id, void *event_data) {
    (void)arg;
    (void)event_data;
    if (s_ap_client_events == NULL || event_base != WIFI_EVENT) {
        return;
    }
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        const uint8_t signal = AP_CLIENT_CONNECTED;
        (void)xQueueSend(s_ap_client_events, &signal, 0);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        const uint8_t signal = AP_CLIENT_DISCONNECTED;
        (void)xQueueSend(s_ap_client_events, &signal, 0);
    }
}

static esp_err_t init_wifi_driver(void) {
    if (s_wifi_initialized) {
        return ESP_OK;
    }

    ensure_server_groups();
    ensure_wifi_events();
    if (s_wifi_events == NULL || s_ap_client_events == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    /* Runtime SD credentials must never be written back into nvs.net80211. */
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        esp_wifi_deinit();
        return err;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &sta_wifi_event_callback, NULL,
                                               &s_wifi_handler);
    if (err != ESP_OK) {
        esp_wifi_deinit();
        return err;
    }
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &sta_wifi_event_callback, NULL,
                                               &s_ip_handler);
    if (err != ESP_OK) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               s_wifi_handler);
        s_wifi_handler = NULL;
        esp_wifi_deinit();
        return err;
    }
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &ap_wifi_event_callback, NULL,
                                               &s_ap_handler);
    if (err != ESP_OK) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               s_ip_handler);
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               s_wifi_handler);
        if (s_ap_handler != NULL) {
            esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   s_ap_handler);
        }
        s_ip_handler = NULL;
        s_wifi_handler = NULL;
        esp_wifi_deinit();
        return err;
    }

    s_wifi_initialized = true;
    return ESP_OK;
}

static void stop_fallback_ap_if_unused(void) {
    if (!s_auto_fallback_ap || !s_ap_active || !s_sta_connected) {
        return;
    }
    wifi_sta_list_t sta_list = {};
    if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
        s_ap_client_count = sta_list.num;
    }
    if (s_ap_client_count != 0) return;
    if (esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK) {
        s_ap_active = false;
    }
}

static void start_fallback_ap(void) {
    if (!s_auto_fallback_ap || s_ap_active || !s_wifi_started) {
        return;
    }
    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK || configure_ap() != ESP_OK) {
        ESP_LOGE(TAG, "Unable to configure the maintenance AP");
        return;
    }
    s_ap_active = true;
    ESP_LOGW(TAG, "STA unavailable; maintenance AP enabled");
}

static bool activate_ap(bool keep_sta) {
    if (!s_wifi_initialized) return false;
    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (s_ap_netif == NULL) return false;
    }
    const wifi_mode_t mode = keep_sta ? WIFI_MODE_APSTA : WIFI_MODE_AP;
    if (esp_wifi_set_mode(mode) != ESP_OK || configure_ap() != ESP_OK) {
        return false;
    }
    if (!s_wifi_started && esp_wifi_start() != ESP_OK) {
        return false;
    }
    s_wifi_started = true;
    s_ap_active = true;
    if (!keep_sta) {
        s_sta_connected = false;
        photopull_network_changed(false);
    }
    return true;
}

static void wifi_maintenance_task(void *arg) {
    (void)arg;
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(
            s_wifi_events,
            WIFI_EVENT_STA_START_BIT | WIFI_EVENT_STA_GOT_IP_BIT |
                WIFI_EVENT_STA_DISCONNECTED_BIT | WIFI_EVENT_MAINTENANCE_REQUEST_BIT,
            pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
        TickType_t now = xTaskGetTickCount();

        uint8_t ap_signal = 0;
        while (xQueueReceive(s_ap_client_events, &ap_signal, 0) == pdTRUE) {
            if (ap_signal == AP_CLIENT_CONNECTED) {
                if (s_ap_client_count < BSP_MAX_STA_CONN) {
                    ++s_ap_client_count;
                }
            } else if (ap_signal == AP_CLIENT_DISCONNECTED) {
                if (s_ap_client_count > 0) {
                    --s_ap_client_count;
                }
            }
        }
        if (bits & WIFI_EVENT_MAINTENANCE_REQUEST_BIT) {
            s_auto_fallback_ap = false;
            s_ap_client_count = 0;
            if (esp_wifi_set_mode(WIFI_MODE_APSTA) == ESP_OK &&
                configure_ap() == ESP_OK) {
                s_ap_active = true;
                ESP_LOGI(TAG, "Maintenance AP enabled by user");
            }
        }
        if (bits & WIFI_EVENT_STA_GOT_IP_BIT) {
            s_sta_connected = true;
            s_retry_delay_ms = WIFI_RETRY_INITIAL_MS;
            photopull_network_changed(true);
            stop_fallback_ap_if_unused();
        }
        if (bits & WIFI_EVENT_STA_DISCONNECTED_BIT) {
            if (s_sta_connected) {
                s_sta_connected = false;
                photopull_network_changed(false);
            }
            schedule_sta_retry(now);
            if (s_auto_fallback_ap) {
                start_fallback_ap();
            }
        }
        stop_fallback_ap_if_unused();

        if (s_wifi_started && !s_sta_connected && s_sta_netif != NULL &&
            tick_reached(now, s_next_retry)) {
            esp_err_t err = esp_wifi_connect();
            if (err == ESP_OK) {
                schedule_sta_retry(now);
            } else {
                schedule_sta_retry(now);
            }
        }
    }
}

static esp_err_t send_builtin_status_page(httpd_req_t *req) {
    static const char kStatusPage[] =
        "<!doctype html><html><head><meta charset=\"utf-8\"><title>PhotoPainter maintenance</title></head>"
        "<body><h1>PhotoPainter maintenance</h1><p>SD card is unavailable. Insert a card and restart.</p></body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, kStatusPage, HTTPD_RESP_USE_STRLEN);
}

static bool is_static_uri(const char *uri) {
    return photopainter_server::IsStaticUri(uri);
}

static const char *content_type_for_uri(const char *uri) {
    if (!strcmp(uri, "/index.html") || !strcmp(uri, "/")) {
        return "text/html; charset=utf-8";
    }
    if (!strcmp(uri, "/bootstrap.min.css") || !strcmp(uri, "/styles.min.css")) {
        return "text/css";
    }
    if (!strcmp(uri, "/placeholder.svg")) {
        return "image/svg+xml";
    }
    return "text/javascript";
}

static const char *storage_path_for_uri(const char *uri) {
    return photopainter_server::StaticStoragePath(uri);
}

static esp_err_t send_static_resource(httpd_req_t *req, const char *uri) {
    if (!is_static_uri(uri)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Resources do not exist");
        return ESP_OK;
    }

    if (!strcmp(uri, "/")) {
        uri = "/index.html";
    }
    const char *storage_path = storage_path_for_uri(uri);
    if (storage_path == NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Resources do not exist");
        return ESP_OK;
    }
    uint8_t *buffer = static_cast<uint8_t *>(heap_caps_malloc(WEB_READ_MAX, MALLOC_CAP_SPIRAM));
    if (buffer == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_OK;
    }
    httpd_resp_set_type(req, content_type_for_uri(uri));

    size_t offset = 0;
    size_t total_size = 0;
    size_t bytes_read = 0;
    for (;;) {
        esp_err_t err = photopull_web_read(storage_path, offset, buffer, WEB_READ_MAX,
                                           &bytes_read, &total_size);
        if (err != ESP_OK) {
            heap_caps_free(buffer);
            if (!strcmp(uri, "/index.html") && offset == 0) {
                return send_builtin_status_page(req);
            }
            if (offset != 0) {
                /* Headers/chunks may already be on the wire.  Finish the
                 * chunked response instead of attempting a second response. */
                (void)httpd_resp_send_chunk(req, NULL, 0);
                return ESP_OK;
            }
            send_unavailable(req,
                                "Storage unavailable");
            return ESP_OK;
        }
        if (bytes_read == 0) {
            break;
        }
        err = httpd_resp_send_chunk(req, reinterpret_cast<const char *>(buffer), bytes_read);
        if (err != ESP_OK) {
            heap_caps_free(buffer);
            return err;
        }
        offset += bytes_read;
        if (total_size != 0 && offset >= total_size) {
            break;
        }
    }
    heap_caps_free(buffer);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t static_resource_unified_handler(httpd_req_t *req) {
    const char *uri = req->uri;
    if (!strcmp(uri, "/NetWorkStatus")) {
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_send(req, s_sta_connected ? staresp : apresp,
                               HTTPD_RESP_USE_STRLEN);
    }
    return send_static_resource(req, uri);
}

static esp_err_t send_upload_error(httpd_req_t *req, esp_err_t err) {
    if (ServerPortGroups != NULL) {
        xEventGroupSetBits(ServerPortGroups, GroupBit1 | GroupBit3);
    }
    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_SIZE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid BMP upload");
    } else if (err == ESP_ERR_INVALID_STATE || err == ESP_ERR_NO_MEM ||
               err == ESP_ERR_TIMEOUT) {
        send_unavailable(req,
                            "Storage busy or unavailable");
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload failed");
    }
    return ESP_OK;
}

static esp_err_t receive_data_redirect_handler(httpd_req_t *req) {
    if (!photopainter_server::IsUploadContentLengthValid(req->content_len)) {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "BMP is too large");
        return ESP_OK;
    }

    uint8_t *buffer = static_cast<uint8_t *>(heap_caps_malloc(WEB_READ_MAX, MALLOC_CAP_SPIRAM));
    if (buffer == NULL) {
        send_unavailable(req, "Out of memory");
        return ESP_OK;
    }

    if (ServerPortGroups != NULL) {
        xEventGroupSetBits(ServerPortGroups, GroupBit0);
    }
    photopainter_server::UploadFraming framing(req->content_len);
    bool upload_started = false;
    uint8_t timeout_count = 0;
    esp_err_t upload_err = ESP_OK;
    while (framing.remaining() > 0) {
        size_t request_size = framing.remaining() < WEB_READ_MAX ? framing.remaining() : WEB_READ_MAX;
        int received = httpd_req_recv(req, reinterpret_cast<char *>(buffer), request_size);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeout_count >= 10) {
                photopull_upload_abort();
                heap_caps_free(buffer);
                if (ServerPortGroups != NULL) {
                    xEventGroupSetBits(ServerPortGroups, GroupBit1 | GroupBit3);
                }
                httpd_resp_send_408(req);
                return ESP_OK;
            }
            continue;
        }
        if (received <= 0 || static_cast<size_t>(received) > framing.remaining()) {
            photopull_upload_abort();
            heap_caps_free(buffer);
            return send_upload_error(req, ESP_ERR_INVALID_ARG);
        }
        timeout_count = 0;
        size_t payload_offset = 0;
        size_t payload_size = 0;
        const bool first_chunk = framing.first_chunk();
        if (!framing.Consume(static_cast<size_t>(received), &payload_offset, &payload_size)) {
            photopull_upload_abort();
            heap_caps_free(buffer);
            return send_upload_error(req, ESP_ERR_INVALID_SIZE);
        }
        if (first_chunk) {
            netMode = buffer[0];
            upload_err = photopull_upload_begin(req->content_len - 1);
            upload_started = (upload_err == ESP_OK);
            if (!upload_started) {
                heap_caps_free(buffer);
                return send_upload_error(req, upload_err);
            }
        }
        if (payload_size > 0) {
            upload_err = photopull_upload_write(buffer + payload_offset, payload_size);
            if (upload_err != ESP_OK) {
                photopull_upload_abort();
                heap_caps_free(buffer);
                return send_upload_error(req, upload_err);
            }
        }
    }

    if (!upload_started) {
        photopull_upload_abort();
        heap_caps_free(buffer);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty upload");
        return ESP_OK;
    }
    upload_err = photopull_upload_finish();
    if (upload_err != ESP_OK) {
        photopull_upload_abort();
        heap_caps_free(buffer);
        return send_upload_error(req, upload_err);
    }
    if (ServerPortGroups != NULL) {
        xEventGroupSetBits(ServerPortGroups, GroupBit1 | GroupBit2);
    }
    heap_caps_free(buffer);
    return httpd_resp_send(req, "Data saved; display request accepted", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t unknown_uri_handler(httpd_req_t *req) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Resources do not exist");
    return ESP_OK;
}

void ServerPort_NetworkAPInit(void) {
    if (init_wifi_driver() != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi driver initialization failed");
        return;
    }
    s_auto_fallback_ap = false;
    s_ap_client_count = 0;
    if (!activate_ap(false)) {
        ESP_LOGE(TAG, "Maintenance AP initialization failed");
        return;
    }
    s_sta_netif = NULL;
    s_sta_connected = false;
    photopull_network_changed(false);
}

uint8_t ServerPort_NetworkSTAInit(wifi_credential_t creden) {
    if (init_wifi_driver() != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi driver initialization failed");
        return 0;
    }
    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }
    const size_t ssid_len = strnlen(creden.ssid, sizeof(creden.ssid));
    const size_t password_len = strnlen(creden.password, sizeof(creden.password));
    if (s_sta_netif == NULL || ssid_len == 0 || ssid_len >= sizeof(creden.ssid) ||
        password_len >= sizeof(creden.password)) {
        ESP_LOGE(TAG, "Invalid STA credentials");
        return 0;
    }

    wifi_config_t wifi_config = {};
    // ESP-IDF accepts full-capacity SSID/password arrays without a trailing
    // NUL. Lengths have been checked above; do not truncate a 32/64-byte value.
    memcpy(wifi_config.sta.ssid, creden.ssid, ssid_len);
    memcpy(wifi_config.sta.password, creden.password, password_len);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    s_auto_fallback_ap = true;
    s_ap_active = false;
    s_sta_connected = false;
    s_ap_client_count = 0;
    xEventGroupClearBits(s_wifi_events, WIFI_EVENT_STA_GOT_IP_BIT |
                                      WIFI_EVENT_STA_DISCONNECTED_BIT |
                                      WIFI_EVENT_STA_START_BIT);
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK ||
        esp_wifi_set_config(WIFI_IF_STA, &wifi_config) != ESP_OK ||
        esp_wifi_start() != ESP_OK) {
        ESP_LOGE(TAG, "STA initialization failed");
        s_wifi_started = false;
        (void)activate_ap(false);
        return 0;
    }
    s_wifi_started = true;
    if (s_wifi_task == NULL) {
        if (xTaskCreate(wifi_maintenance_task, "wifi_maintenance", 4096, NULL, 4,
                        &s_wifi_task) != pdPASS) {
            s_wifi_task = NULL;
            ESP_LOGE(TAG, "Wi-Fi maintenance task creation failed");
            (void)activate_ap(false);
            return 0;
        }
    }
    const TickType_t started_at = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(WIFI_FIRST_CONNECT_TIMEOUT_MS);
    while (!s_sta_connected && !tick_reached(xTaskGetTickCount(), started_at + timeout)) {
        /* The event callback owns the atomic connection state.  Polling it
         * avoids one consumer clearing the event before the maintenance task
         * sees it. */
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (s_sta_connected) {
        return 1;
    }
    start_fallback_ap();
    return 0;
}

void ServerPort_init(CustomSDPort *SDPort) {
    (void)SDPort;
    ensure_server_groups();
    if (s_http_server != NULL) {
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;
    config.max_uri_handlers = 8;
    if (httpd_start(&s_http_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Web server initialization failed");
        return;
    }

    httpd_uri_t uri_config = {};
    uri_config.uri = "/*";
    uri_config.method = HTTP_GET;
    uri_config.handler = static_resource_unified_handler;
    if (httpd_register_uri_handler(s_http_server, &uri_config) != ESP_OK) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
        return;
    }

    uri_config = {};
    uri_config.uri = "/dataUP";
    uri_config.method = HTTP_POST;
    uri_config.handler = receive_data_redirect_handler;
    if (httpd_register_uri_handler(s_http_server, &uri_config) != ESP_OK) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
        return;
    }

    uri_config = {};
    uri_config.uri = "/*";
    /* httpd_method_t is an enum, not a bitmask: GET|POST evaluates to POST.
     * GET is already covered by the static handler above. */
    uri_config.method = HTTP_POST;
    uri_config.handler = unknown_uri_handler;
    if (httpd_register_uri_handler(s_http_server, &uri_config) != ESP_OK) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
        return;
    }
    s_http_ready = true;
}

bool ServerPort_ready(void) {
    return s_http_ready && s_wifi_started && (s_ap_active || s_sta_connected);
}

void ServerPort_SetNetworkSleep(void) {
    /* Kept for source compatibility; Network mode no longer calls this path. */
    if (s_wifi_started) {
        esp_wifi_stop();
        s_wifi_started = false;
        s_sta_connected = false;
        s_ap_active = false;
        photopull_network_changed(false);
    }
}

void ServerPort_EnterMaintenanceAp(void) {
    if (!s_wifi_initialized) {
        ServerPort_NetworkAPInit();
        return;
    }
    if (s_wifi_task == NULL) {
        s_auto_fallback_ap = false;
        const bool keep_sta = s_sta_netif != NULL && s_wifi_started;
        if (!activate_ap(keep_sta)) {
            ESP_LOGE(TAG, "Unable to enable the maintenance AP");
        }
        return;
    }
    /* The Wi-Fi task performs the mode change outside the button task. */
    xEventGroupSetBits(s_wifi_events, WIFI_EVENT_MAINTENANCE_REQUEST_BIT);
}

uint8_t Get_NetworkMode(void) {
    return netMode;
}

void Mdns_init_config(void) {
    esp_err_t err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "mDNS initialization failed: %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set("esp32-s3-photopainter");
    mdns_instance_name_set("ESP32-S3 WebServer");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
}
