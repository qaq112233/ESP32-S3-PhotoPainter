#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include "sdkconfig.h"
#include "user_app.h"
#include "button_bsp.h"

#if CONFIG_PHOTOPAINTER_ENABLE_XIAOZHI

#include <esp_heap_caps.h>
#include <driver/rtc_io.h>
#include "codec_bsp.h"

CodecPort *AudioPort = NULL;
EventGroupHandle_t audio_groups;

static void key1_button_user_Task(void *arg) {
    esp_err_t ret;
    uint8_t   Mode = 0;
    for (;;) {
        EventBits_t even = xEventGroupWaitBits(GP4ButtonGroups, (0x02) | (0x01), pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));
        if (get_bit_button(even, 1)) {
            if (Mode > 0) {
                nvs_handle_t my_handle;
                ret = nvs_open("PhotoPainter", NVS_READWRITE, &my_handle);
                ESP_ERROR_CHECK(ret);
                vTaskDelay(pdMS_TO_TICKS(2));
                if (Mode == 1) {
                    ret = nvs_set_u8(my_handle, "PhotPainterMode", 0x01);
                    ESP_ERROR_CHECK(ret);
                    vTaskDelay(pdMS_TO_TICKS(2));
                } else if (Mode == 2) {
                    ret = nvs_set_u8(my_handle, "PhotPainterMode", 0x02);
                    ESP_ERROR_CHECK(ret);
                    vTaskDelay(pdMS_TO_TICKS(2));
                } else if (Mode == 3) {
                    ret = nvs_set_u8(my_handle, "PhotPainterMode", 0x03);
                    ESP_ERROR_CHECK(ret);
                    vTaskDelay(pdMS_TO_TICKS(2));
                }
                ret = nvs_set_u8(my_handle, "Mode_Flag", 0x01);
                ESP_ERROR_CHECK(ret);
                vTaskDelay(pdMS_TO_TICKS(2));
                ESP_LOGW("Audio", "Mode selection is committed");
                ESP_ERROR_CHECK(nvs_commit(my_handle));
                nvs_close(my_handle);
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_restart();
            }
        } else if (get_bit_button(even, 0)) {
            Mode++;
            if (Mode > 3) {
                Mode = 1;
            }
            if (Mode == 1) {
                xEventGroupSetBits(audio_groups, set_bit_button(1));
            } else if (Mode == 2) {
                xEventGroupSetBits(audio_groups, set_bit_button(2));
            } else if (Mode == 3) {
                xEventGroupSetBits(audio_groups, set_bit_button(3));
            }
        }
    }
}

static void audio_user_Task(void *arg) {
    AudioPort->Codec_PlayInfoAudio();
    int value = 0;
    for (;;) {
        EventBits_t even = xEventGroupWaitBits(audio_groups, set_bit_all, pdTRUE, pdFALSE, pdMS_TO_TICKS(3000));
        if (get_bit_button(even, 0)) {
            value = 0;
        } else if (get_bit_button(even, 1)) {
            value = 1;
        } else if (get_bit_button(even, 2)) {
            value = 2;
        } else if (get_bit_button(even, 3)) {
            value = 3;
        }
        int      bytes_write = 0;
        int      bytes_sizt  = AudioPort->Codec_GetMusicSizt(value);
        uint8_t *Music_ptr   = AudioPort->Codec_GetMusicData(value);
        do {
            AudioPort->Codec_PlayBackWrite(Music_ptr, 256);
            Music_ptr += 256;
            bytes_write += 256;
        } while ((bytes_write < bytes_sizt) && (gpio_get_level(GPIO_NUM_4)));
    }
}

bool Mode_Selection_Init(void) {
    AudioPort    = new CodecPort(I2cBus);
    audio_groups = xEventGroupCreate();
    if (AudioPort == NULL || audio_groups == NULL) {
        ESP_LOGE("ModeSelection", "Unable to initialize audio mode selection");
        return false;
    }
    xEventGroupSetBits(audio_groups, set_bit_button(0));
    const BaseType_t key_task = xTaskCreate(
        key1_button_user_Task, "key1_button_user_Task", 4 * 1024, NULL, 3, NULL);
    const BaseType_t audio_task = xTaskCreate(
        audio_user_Task, "audio_user_Task", 4 * 1024, NULL, 3, NULL);
    if (key_task != pdPASS || audio_task != pdPASS) {
        ESP_LOGE("ModeSelection", "Unable to create audio mode selection tasks");
        return false;
    }
    return true;
}

#else

/* Lite mode selection deliberately has no codec construction or audio task.
 * The existing LEDs provide one blink for Basic and two for Network. */
static void lite_mode_selection_task(void *arg) {
    uint8_t mode = 0;
    for (;;) {
        EventBits_t events = xEventGroupWaitBits(
            GP4ButtonGroups, 0x02 | 0x01, pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));

        if (get_bit_button(events, 1)) {
            if (mode == 0) {
                continue;
            }

            nvs_handle_t handle;
            esp_err_t err = nvs_open("PhotoPainter", NVS_READWRITE, &handle);
            if (err != ESP_OK) {
                ESP_LOGE("ModeSelection", "Cannot open mode storage: %s", esp_err_to_name(err));
                continue;
            }
            err = nvs_set_u8(handle, "PhotPainterMode", mode);
            if (err == ESP_OK) {
                err = nvs_set_u8(handle, "Mode_Flag", 0x01);
            }
            if (err == ESP_OK) {
                err = nvs_commit(handle);
            }
            nvs_close(handle);
            if (err != ESP_OK) {
                ESP_LOGE("ModeSelection", "Cannot save selected mode: %s", esp_err_to_name(err));
                continue;
            }
            ESP_LOGI("ModeSelection", "Selected %s mode", mode == 1 ? "Basic" : "Network");
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }

        if (get_bit_button(events, 0)) {
            mode = (mode == 1) ? 2 : 1;
            xEventGroupSetBits(Green_led_Mode_queue, set_bit_button(mode));
        }
    }
}

bool Mode_Selection_Init(void) {
    return xTaskCreate(lite_mode_selection_task, "mode_selection_task", 3 * 1024,
                       NULL, 3, NULL) == pdPASS;
}

#endif
