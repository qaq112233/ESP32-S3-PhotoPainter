#include <stdio.h>
#include <string.h>
#include <esp_heap_caps.h>
#include <nvs_flash.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_log.h>
#include <esp_err.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <cJSON.h>
#include "user_app.h"
#include "button_bsp.h"
#include "list.h"


#define ext_wakeup_pin_1 GPIO_NUM_0
#define ext_wakeup_pin_3 GPIO_NUM_4 

static RTC_DATA_ATTR uint32_t sdcard_Basic_count = 0; 
static RTC_DATA_ATTR uint32_t basic_rtc_set_time = 13 * 60;// User sets the wake-up time in seconds. // The default is 60 seconds. It is awakened by a timer.
static uint8_t           Basic_sleep_arg = 0; // Parameters for low-power tasks
static SemaphoreHandle_t sleep_Semp;          // Binary call low-power task 
static uint8_t           wakeup_basic_flag = 0;
static list_t* ListHost;

/* Basic mode keeps the legacy JSON file and timer field, but its timer is a
 * local display setting and must not depend on the AI configuration parser.
 * Missing, malformed or non-positive values retain the legacy default. */
static uint32_t load_basic_timer_seconds(void) {
    static const uint32_t kDefaultTimerSeconds = 13U * 60U;
    static const size_t kMaxConfigSize = 8U * 1024U;
    const char *path = "/sdcard/06_user_foundation_img/config.txt";
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return kDefaultTimerSeconds;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return kDefaultTimerSeconds;
    }
    long file_size = ftell(file);
    if (file_size <= 0 || (size_t)file_size > kMaxConfigSize || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return kDefaultTimerSeconds;
    }

    char *contents = (char *)malloc((size_t)file_size + 1U);
    if (contents == NULL) {
        fclose(file);
        return kDefaultTimerSeconds;
    }
    size_t bytes_read = fread(contents, 1, (size_t)file_size, file);
    fclose(file);
    if (bytes_read != (size_t)file_size) {
        free(contents);
        return kDefaultTimerSeconds;
    }
    contents[bytes_read] = '\0';

    cJSON *root = cJSON_ParseWithLength(contents, bytes_read);
    free(contents);
    if (root == NULL) {
        ESP_LOGW("TIMER", "Basic timer config is invalid; using default");
        return kDefaultTimerSeconds;
    }

    cJSON *timer = cJSON_GetObjectItemCaseSensitive(root, "timer");
    uint32_t result = kDefaultTimerSeconds;
    if (cJSON_IsNumber(timer) && timer->valuedouble > 0.0 && timer->valuedouble <= UINT32_MAX) {
        result = (uint32_t)timer->valuedouble;
    }
    cJSON_Delete(root);
    return result;
}

static void boot_button_user_Task(void *arg) {
    uint8_t *wakeup_arg = (uint8_t *) arg;
    for (;;) {
        EventBits_t even = xEventGroupWaitBits(BootButtonGroups, (0x01) | (0x02) , pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));
        if (get_bit_button(even, 0)) { //单击
            if (*wakeup_arg == 0) {
                if (pdTRUE == xSemaphoreTake(epaper_gui_semapHandle, 2000)) {                       
                    list_node_t *sdcard_node = list_at(ListHost, sdcard_Basic_count); 
                    if (sdcard_node == NULL) {
                        sdcard_Basic_count = 0;
                        sdcard_node        = list_at(ListHost, sdcard_Basic_count);
                    }
                    ESP_LOGW("node", "%ld", sdcard_Basic_count);
                    sdcard_Basic_count++;
                    if (sdcard_node != NULL) {
                        xEventGroupSetBits(Green_led_Mode_queue,set_bit_button(6));
                        Green_led_arg                   = 1;
                        CustomSDPortNode_t *sdcard_Name_node = (CustomSDPortNode_t *) sdcard_node->val;
                        const esp_err_t decode_ret = ePaperDisplay.EPD_SDcardScaleIMGShakingColor(
                            sdcard_Name_node->sdcard_name, 0, 0);
                        const esp_err_t refresh_ret = decode_ret == ESP_OK
                            ? ePaperDisplay.EPD_Display() : decode_ret;
                        xSemaphoreGive(epaper_gui_semapHandle); 
                        Green_led_arg = 0;
                        if (refresh_ret == ESP_OK) {
                            Basic_sleep_arg = 1;
                            xSemaphoreGive(sleep_Semp);
                        } else {
                            /* A failed refresh must leave the panel available
                             * for recovery instead of entering deep sleep. */
                            Basic_sleep_arg = 0;
                            ESP_LOGE("Basic", "Photo refresh failed: %s",
                                     esp_err_to_name(refresh_ret));
                        }
                    } else {
                        /* Do not strand the display mutex when the list is
                         * empty or the SD card disappeared. */
                        xSemaphoreGive(epaper_gui_semapHandle);
                    }
                }
            }
        } else if(even & 0x02) { //长按 低功耗
            const uint64_t ext_wakeup_pin_1_mask = 1ULL << ext_wakeup_pin_1;
            const uint64_t ext_wakeup_pin_3_mask = 1ULL << ext_wakeup_pin_3;
            ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(ext_wakeup_pin_1_mask | ext_wakeup_pin_3_mask, ESP_EXT1_WAKEUP_ANY_LOW)); 
            ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(ext_wakeup_pin_3));
            ESP_ERROR_CHECK(rtc_gpio_pullup_en(ext_wakeup_pin_3));
            esp_sleep_enable_timer_wakeup((uint64_t)basic_rtc_set_time * 1000000ULL);
            //axp_basic_sleep_start();
            do {
                vTaskDelay(pdMS_TO_TICKS(50));
            } while (gpio_get_level(ext_wakeup_pin_1) == 0);
            esp_deep_sleep_start();
        }
    }
}

static void default_sleep_user_Task(void *arg) {
    uint8_t *sleep_arg = (uint8_t *) arg;
    for (;;) {
        if (pdTRUE == xSemaphoreTake(sleep_Semp, portMAX_DELAY)) {
            if (*sleep_arg == 1) {
                const uint64_t ext_wakeup_pin_1_mask = 1ULL << ext_wakeup_pin_1;
                const uint64_t ext_wakeup_pin_3_mask = 1ULL << ext_wakeup_pin_3;
                ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(ext_wakeup_pin_1_mask | ext_wakeup_pin_3_mask,ESP_EXT1_WAKEUP_ANY_LOW)); 
                ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(ext_wakeup_pin_3));
                ESP_ERROR_CHECK(rtc_gpio_pullup_en(ext_wakeup_pin_3));
                esp_sleep_enable_timer_wakeup((uint64_t)basic_rtc_set_time * 1000000ULL);
                //axp_basic_sleep_start(); 
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_deep_sleep_start();  
            }
        }
    }
}

static void get_wakeup_gpio(void) {
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
    if (ESP_SLEEP_WAKEUP_EXT1 == wakeup_reason) {
        uint64_t wakeup_pins = esp_sleep_get_ext1_wakeup_status();
        if (wakeup_pins == 0)
            return;
        if (wakeup_pins & (1ULL << ext_wakeup_pin_1)) {
            xEventGroupSetBits(BootButtonGroups, set_bit_button(0)); 
        } else if (wakeup_pins & (1ULL << ext_wakeup_pin_3)) {
            return;
        }
    } else if (ESP_SLEEP_WAKEUP_TIMER == wakeup_reason) {
        xEventGroupSetBits(BootButtonGroups, set_bit_button(0)); 
    }
}

bool User_Basic_mode_app_init(void) {
    if (SDPort == NULL || epaper_gui_semapHandle == NULL ||
        Green_led_Mode_queue == NULL || Red_led_Mode_queue == NULL ||
        BootButtonGroups == NULL) {
        ESP_LOGE("Basic", "Basic mode prerequisites are unavailable");
        return false;
    }

    ListHost = SDPort->SDPort_GetListHost();
    sleep_Semp  = xSemaphoreCreateBinary();
    if (sleep_Semp == NULL) {
        ESP_LOGE("Basic", "Unable to allocate sleep semaphore");
        return false;
    }
    xEventGroupSetBits(Red_led_Mode_queue, set_bit_button(0));
    basic_rtc_set_time = load_basic_timer_seconds();
    ESP_LOGI("TIMER", "basic_rtc_set_time:%lu", (unsigned long)basic_rtc_set_time);
    SDPort->SDPort_ScanListDir("/sdcard/06_user_foundation_img"); 
    ESP_LOGW("IMG","Values:%d",SDPort->Get_Sdcard_ImgValue());  
    TaskHandle_t boot_task = NULL;
    TaskHandle_t sleep_task = NULL;
    const BaseType_t boot_created = xTaskCreate(
        boot_button_user_Task, "boot_button_user_Task", 6 * 1024,
        &wakeup_basic_flag, 3, &boot_task);
    const BaseType_t sleep_created = xTaskCreate(
        default_sleep_user_Task, "default_sleep_user_Task", 4 * 1024,
        &Basic_sleep_arg, 3, &sleep_task);
    if (boot_created != pdPASS || sleep_created != pdPASS) {
        if (boot_created == pdPASS) vTaskDelete(boot_task);
        if (sleep_created == pdPASS) vTaskDelete(sleep_task);
        ESP_LOGE("Basic", "Unable to create Basic mode tasks");
        return false;
    }
    get_wakeup_gpio();
    return true;
}
