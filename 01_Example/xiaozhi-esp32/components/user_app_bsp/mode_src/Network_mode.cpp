#include <stdio.h>
#include <string.h>

#include <esp_err.h>
#include <esp_log.h>
#include <nvs.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "button_bsp.h"
#include "server_app.h"
#include "service.h"
#include "traverse_nvs.h"
#include "user_app.h"

TraverseNvs *nvs_viewer = NULL;
static const char *TAG = "NetworkMode";
static uint8_t NetWorkMode = 0;

uint8_t Get_nvsNetworkMode(void) {
    nvs_handle_t handle;
    if (nvs_open("PhotoPainter", NVS_READONLY, &handle) != ESP_OK) {
        return 0;
    }
    uint8_t mode = 0;
    if (nvs_get_u8(handle, "NetworkMode", &mode) != ESP_OK) {
        mode = 0;
    }
    nvs_close(handle);
    return mode;
}

void Set_nvsNetworkMode(uint8_t mode) {
    nvs_handle_t handle;
    if (nvs_open("PhotoPainter", NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "Unable to open mode settings");
        return;
    }
    uint8_t current = 0;
    esp_err_t err = nvs_get_u8(handle, "NetworkMode", &current);
    if (err == ESP_ERR_NVS_NOT_FOUND || current != mode) {
        err = nvs_set_u8(handle, "NetworkMode", mode);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
    }
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Unable to save mode setting: %s", esp_err_to_name(err));
    }
    nvs_close(handle);
}

uint8_t Get_CurrentlyNetworkMode(void) {
    return NetWorkMode;
}

/* The Web task reports upload progress through these legacy LED event bits. */
static void Network_user_Task(void *arg) {
    (void)arg;
    for (;;) {
        EventBits_t events = xEventGroupWaitBits(
            ServerPortGroups, GroupBit0 | GroupBit1 | GroupBit2 | GroupBit3,
            pdTRUE, pdFALSE, portMAX_DELAY);
        if (events & GroupBit0) {
            Red_led_arg = 1;
            xEventGroupSetBits(Red_led_Mode_queue, set_bit_button(6));
        }
        if (events & GroupBit1) {
            Red_led_arg = 0;
        }
        if (events & GroupBit2) {
            /* The manager owns the accepted upload and display request. */
            xEventGroupSetBits(Green_led_Mode_queue, set_bit_button(1));
            if (Get_NetworkMode() <= 1 && Get_NetworkMode() != NetWorkMode) {
                /* The UI applies the selected AP/STA mode after the next reboot. */
                Set_nvsNetworkMode(Get_NetworkMode());
            }
        }
        if (events & GroupBit3) {
            xEventGroupSetBits(Green_led_Mode_queue, set_bit_button(2));
        }
    }
}

static void boot_button_click_Task(void *arg) {
    (void)arg;
    for (;;) {
        EventBits_t events = xEventGroupWaitBits(
        BootButtonGroups, GroupBit0, pdTRUE, pdFALSE, portMAX_DELAY);
        if (events & GroupBit0) {
            /* Keep the running service alive while entering maintenance AP. */
            ServerPort_EnterMaintenanceAp();
        }
    }
}

static bool wait_for_photopull_ready(bool already_started) {
    if (!already_started) {
        ESP_LOGW(TAG, "PhotoPull service did not start; maintenance remains available");
        return false;
    }
    /* Recovery validates complete BMPs before advertising readiness; 50 files
     * can take substantially longer than a network timeout on a busy card. */
    constexpr TickType_t kReadyTimeout = pdMS_TO_TICKS(120000);
    TickType_t deadline = xTaskGetTickCount() + kReadyTimeout;
    while (!photopull_ready()) {
        TickType_t now = xTaskGetTickCount();
        if (static_cast<int32_t>(now - deadline) >= 0) {
            ESP_LOGW(TAG, "PhotoPull service readiness timed out");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return true;
}

bool User_Network_mode_app_init(void) {
    NetWorkMode = Get_nvsNetworkMode();
    const bool service_started = photopull_start();
    const bool storage_available = SDPort != NULL &&
                                   SDPort->SDPort_GetSdcardInitOK() != 0;

    wifi_credential_t credentials = {};
    char ssid[sizeof(credentials.ssid)] = {};
    char password[sizeof(credentials.password)] = {};
    /* Recovery owns the SD files while it validates snapshots and schedules
     * the carousel.  Keep Wi-Fi and HTTP startup behind this bounded local
     * readiness gate in the normal path. */
    const bool service_ready = wait_for_photopull_ready(service_started);
    if (storage_available &&
        photopull_wifi_credentials(ssid, sizeof(ssid), password, sizeof(password))) {
        /* SD credentials are for this run only and are never copied to NVS. */
        snprintf(credentials.ssid, sizeof(credentials.ssid), "%s", ssid);
        snprintf(credentials.password, sizeof(credentials.password), "%s", password);
        credentials.is_valid = true;
    } else if (storage_available) {
        nvs_viewer = new TraverseNvs();
        credentials = nvs_viewer->Get_WifiCredentialFromNVS();
    }

    if (!storage_available) {
        /* A missing SD card is a maintenance condition.  Do not use stale
         * nvs.net80211 credentials to enter STA mode for this boot. */
        ESP_LOGW(TAG, "SD card unavailable; forcing maintenance AP");
        ServerPort_NetworkAPInit();
        NetWorkMode = 0;
    } else if (credentials.is_valid) {
        /* A failed STA attempt leaves the automatic maintenance AP running. */
        NetWorkMode = 1;
        ServerPort_NetworkSTAInit(credentials);
    } else {
        ServerPort_NetworkAPInit();
    }
    Mdns_init_config();
    ServerPort_init(SDPort);
    const bool groups_ready = ServerPortGroups != NULL && BootButtonGroups != NULL &&
                              Red_led_Mode_queue != NULL && Green_led_Mode_queue != NULL;
    bool tasks_ready = false;
    if (groups_ready) {
        if (Red_led_Mode_queue != NULL) {
            xEventGroupSetBits(Red_led_Mode_queue, set_bit_button(0));
        }
        TaskHandle_t upload_task = NULL;
        TaskHandle_t button_task = NULL;
        const BaseType_t upload_created = xTaskCreate(
            Network_user_Task, "Network_upload_status", 4 * 1024,
            NULL, 2, &upload_task);
        const BaseType_t button_created = xTaskCreate(
            boot_button_click_Task, "boot_button_click_Task", 4 * 1024,
            NULL, 3, &button_task);
        tasks_ready = upload_created == pdPASS && button_created == pdPASS;
        if (!tasks_ready) {
            ESP_LOGE(TAG, "Network task creation failed");
        }
    }
    const bool web_ready = ServerPort_ready();
    return service_ready && web_ready && tasks_ready;
}
