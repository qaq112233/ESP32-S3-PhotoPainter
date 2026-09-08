#include <driver/gpio.h>
#include <esp_err.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <string.h>

#include "sdkconfig.h"
#include "service.h"
#include "business_mode.h"
#include "user_app.h"

#if CONFIG_PHOTOPAINTER_ENABLE_XIAOZHI
#include "application.h"
#endif

#define TAG "main"

static bool load_or_select_mode(nvs_handle_t handle, uint8_t *mode) {
    uint8_t stored_mode = 0;
    esp_err_t err = nvs_get_u8(handle, "PhotPainterMode", &stored_mode);
    if (err == ESP_OK) {
        *mode = stored_mode;
        return true;
    }
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Unable to read saved mode: %s", esp_err_to_name(err));
    }

    /* Both images choose their first business mode from the same SD-backed
     * PhotoPull configuration. Do not persist the temporary value until after
     * User_Mode_init has mounted the card and the configuration is loaded. */
    *mode = 0x01;
    return false;
}

static void confirm_ota_boot(bool mode_ready) {
    if (!mode_ready) {
        return;
    }

    const esp_partition_t *partition = esp_ota_get_running_partition();
    if (partition == NULL || strcmp(partition->label, "factory") == 0) {
        return;
    }

    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(partition, &state) != ESP_OK) {
        ESP_LOGW(TAG, "Unable to read OTA state for %s", partition->label);
        return;
    }
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Unable to confirm OTA image: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "Application startup confirmed");
        }
    }
}

extern "C" void app_main(void) {
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    nvs_handle_t handle;
    ESP_ERROR_CHECK(nvs_open("PhotoPainter", NVS_READWRITE, &handle));

    uint8_t network_mode = 0;
    if (nvs_get_u8(handle, "NetworkMode", &network_mode) != ESP_OK) {
        ESP_ERROR_CHECK(nvs_set_u8(handle, "NetworkMode", 0x00));
    }

    uint8_t mode = 0x01;
    bool has_saved_mode = load_or_select_mode(handle, &mode);

    uint8_t mode_flag = 0x01;
    if (nvs_get_u8(handle, "Mode_Flag", &mode_flag) != ESP_OK) {
        ESP_ERROR_CHECK(nvs_set_u8(handle, "Mode_Flag", 0x01));
    }
    ESP_ERROR_CHECK(nvs_commit(handle));
    nvs_close(handle);

    const uint8_t init_ok = User_Mode_init();

    const bool storage_available = SDPort != NULL && SDPort->SDPort_GetSdcardInitOK() != 0;
    /* Configuration is read only after the SD mount attempt and before the
     * mode dispatch. A missing card therefore cannot erase local state. */
    photopull_load_config(storage_available);

    const auto selected = photopull::ChooseBusinessMode(has_saved_mode, mode,
        photopull_default_network(),
#if CONFIG_PHOTOPAINTER_ENABLE_XIAOZHI
        true,
#else
        false,
#endif
        storage_available);
    mode = selected.saved_value;
    if (selected.persist) {
        nvs_handle_t settings;
        esp_err_t saved = nvs_open("PhotoPainter", NVS_READWRITE, &settings);
        if (saved == ESP_OK) {
            saved = nvs_set_u8(settings, "PhotPainterMode", mode);
            if (saved == ESP_OK) saved = nvs_commit(settings);
            nvs_close(settings);
        }
        if (saved != ESP_OK) ESP_LOGW(TAG, "Unable to persist business mode: %s", esp_err_to_name(saved));
    }

    if (init_ok == 0) {
        ESP_LOGE(TAG, "Hardware/application initialization failed");
        return;
    }

    const uint8_t boot_mode = selected.boot_value;
    if (!storage_available) {
        ESP_LOGW(TAG, "SD unavailable; entering Network maintenance for this boot");
    }

    bool mode_ready = false;
    if (boot_mode == 0x03) {
#if CONFIG_PHOTOPAINTER_ENABLE_XIAOZHI
        ESP_LOGW(TAG, "Enter xiaozhi mode");
        auto &app = Application::GetInstance();
        app.Start();
        mode_ready = true;
#else
        /* The migration above normally handles this branch. Keep a safe
         * fallback if storage could not be updated. */
        mode = 0x02;
        mode_ready = User_Network_mode_app_init();
#endif
    } else if (boot_mode == 0x01) {
        ESP_LOGW(TAG, "Enter Basic mode");
        mode_ready = User_Basic_mode_app_init();
    } else if (boot_mode == 0x02) {
        ESP_LOGW(TAG, "Enter Network mode");
        mode_ready = User_Network_mode_app_init();
    } else if (boot_mode == 0x04) {
        ESP_LOGW(TAG, "Enter Mode Selection");
        mode_ready = Mode_Selection_Init();
    } else {
        ESP_LOGW(TAG, "Unknown saved mode %u; entering Basic mode", boot_mode);
        mode_ready = User_Basic_mode_app_init();
    }

    confirm_ota_boot(mode_ready);
}
