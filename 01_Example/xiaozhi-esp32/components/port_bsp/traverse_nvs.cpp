#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <nvs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "traverse_nvs.h"

TraverseNvs::TraverseNvs() {
}

TraverseNvs::~TraverseNvs() {
}

void TraverseNvs::TraverseNvs_PrintAllNvs(const char *ns_name) {
    nvs_iterator_t it = NULL;
    esp_err_t      err;

    err = nvs_entry_find(NVS_DEFAULT_PART_NAME, ns_name, NVS_TYPE_ANY, &it);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGI(TAG, "No data was found in NVS (namespace: %s)", ns_name ? ns_name : "All");
            return;
        }
        ESP_LOGE(TAG, "NVS iterator creation failed: %s", esp_err_to_name(err));
        return;
    }

    nvs_entry_info_t info;
    int              entry_count = 0;
    while (it != NULL) {
        nvs_entry_info(it, &info);
        entry_count++;

        ESP_LOGI(TAG, "\n===== Item %d =====", entry_count);
        ESP_LOGI(TAG, "Namespace: %s", info.namespace_name);
        ESP_LOGI(TAG, "Key name: %s", info.key);

        nvs_handle_t handle;
        err = nvs_open(info.namespace_name, NVS_READONLY, &handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open the namespace: %s", esp_err_to_name(err));
            err = nvs_entry_next(&it);
            if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
                ESP_LOGE(TAG, "Failed to iterate to the next item: %s", esp_err_to_name(err));
            }
            continue;
        }

        switch (info.type) {
        case NVS_TYPE_U8: {
            uint8_t val;
            nvs_get_u8(handle, info.key, &val);
            ESP_LOGI(TAG, "Type: uint8_t | Value: %u", val);
            break;
        }
        case NVS_TYPE_I8: {
            int8_t val;
            nvs_get_i8(handle, info.key, &val);
            ESP_LOGI(TAG, "Type: int8_t | Value: %d", val);
            break;
        }
        case NVS_TYPE_U16: {
            uint16_t val;
            nvs_get_u16(handle, info.key, &val);
            ESP_LOGI(TAG, "Type: uint16_t | Value: %u", val);
            break;
        }
        case NVS_TYPE_I16: {
            int16_t val;
            nvs_get_i16(handle, info.key, &val);
            ESP_LOGI(TAG, "Type: int16_t | Value: %d", val);
            break;
        }
        case NVS_TYPE_U32: {
            uint32_t val;
            nvs_get_u32(handle, info.key, &val);
            ESP_LOGI(TAG, "命名空间: %s", info.namespace_name);
            ESP_LOGI(TAG, "Type: uint32_t | Value: %u", val);
            break;
        }
        case NVS_TYPE_I32: {
            int32_t val;
            nvs_get_i32(handle, info.key, &val);
            ESP_LOGI(TAG, "Type: int32_t | Value: %d", val);
            break;
        }
        case NVS_TYPE_U64: {
            uint64_t val;
            nvs_get_u64(handle, info.key, &val);
            ESP_LOGI(TAG, "Type: uint64_t | Value: %llu", val);
            break;
        }
        case NVS_TYPE_I64: {
            int64_t val;
            nvs_get_i64(handle, info.key, &val);
            ESP_LOGI(TAG, "Type: int64_t | Value: %lld", val);
            break;
        }
        case NVS_TYPE_STR: {
            size_t len;
            if (nvs_get_str(handle, info.key, NULL, &len) == ESP_OK) {
                /* NVS may contain Wi-Fi passwords or service tokens. */
                ESP_LOGI(TAG, "Type: string | Length: %zu | Value: <redacted>", len);
            }
            break;
        }
        case NVS_TYPE_BLOB: {
            size_t len;
            if (nvs_get_blob(handle, info.key, NULL, &len) == ESP_OK) {
                /* Blobs in nvs.net80211 include the STA credentials. */
                ESP_LOGI(TAG, "Type: blob | Length: %zu | Value: <redacted>", len);
            }
            break;
        }
        default:
            ESP_LOGI(TAG, "Type: Unknown | Value: Unable to read");
            break;
        }
        nvs_close(handle);
        err = nvs_entry_next(&it);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to iterate to the next item: %s", esp_err_to_name(err));
            break;
        }
    }

    if (it != NULL) {
        nvs_release_iterator(it);
    }
    ESP_LOGI(TAG, "Traversal completed. A total of %d NVS entries were found.", entry_count);
}

bool TraverseNvs::parse_sta_ssid_blob(const uint8_t *blob_data, size_t blob_len, char *out_ssid) {
    if (blob_data == NULL || blob_len < 4 || out_ssid == NULL) {
        if (out_ssid != NULL) {
            out_ssid[0] = '\0';
        }
        return false;
    }
    uint32_t ssid_len = 0;
    memcpy(&ssid_len, blob_data, sizeof(ssid_len));
    if (ssid_len == 0 || ssid_len > 32 || ssid_len > blob_len - 4) {
        out_ssid[0] = '\0';
        return false;
    }
    memcpy(out_ssid, blob_data + 4, ssid_len);
    out_ssid[ssid_len] = '\0';
    return true;
}

bool TraverseNvs::parse_sta_pswd_blob(const uint8_t *blob_data, size_t blob_len, char *out_password) {
    if (out_password == NULL || blob_len > 64 || (blob_data == NULL && blob_len != 0)) {
        if (out_password != NULL) {
            out_password[0] = '\0';
        }
        return false;
    }

    size_t pswd_len = 0;
    while (pswd_len < blob_len) {
        if (blob_data[pswd_len] == '\0') {
            break;
        }
        out_password[pswd_len] = blob_data[pswd_len];
        pswd_len++;
    }

    out_password[pswd_len] = '\0';
    return true;
}

wifi_credential_t TraverseNvs::Get_WifiCredentialFromNVS(void) {
    wifi_credential_t cred = {0};
    cred.is_valid          = false;

    nvs_handle_t handle;
    esp_err_t    err = nvs_open("nvs.net80211", NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open the NVS namespace nvs.net80211: %s", esp_err_to_name(err));
        return cred;
    }

    size_t ssid_blob_len = 0;
    
    err = nvs_get_blob(handle, "sta.ssid", NULL, &ssid_blob_len);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGE(TAG, "The key \"sta.ssid\" was not found in NVS.");
        } else {
            ESP_LOGE(TAG, "Failed to read the length of sta.ssid: %s", esp_err_to_name(err));
        }
        nvs_close(handle);
        return cred;
    }

    
    if (ssid_blob_len < 4 || ssid_blob_len > 36) {
        ESP_LOGE(TAG, "Invalid STA SSID data length");
        nvs_close(handle);
        return cred;
    }
    uint8_t *ssid_blob = (uint8_t *) malloc(ssid_blob_len);
    if (ssid_blob == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for the SSID blob");
        nvs_close(handle);
        return cred;
    }
    err = nvs_get_blob(handle, "sta.ssid", ssid_blob, &ssid_blob_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read the sta.ssid blob data: %s", esp_err_to_name(err));
        free(ssid_blob);
        nvs_close(handle);
        return cred;
    }
    
    bool ssid_valid = parse_sta_ssid_blob(ssid_blob, ssid_blob_len, cred.ssid);
    free(ssid_blob); 

    size_t pswd_blob_len = 0;
    err = nvs_get_blob(handle, "sta.pswd", NULL, &pswd_blob_len);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGE(TAG, "The \"sta.pswd\" key was not found in NVS.");
        } else {
            ESP_LOGE(TAG, "Failed to read the length of sta.pswd: %s", esp_err_to_name(err));
        }
        nvs_close(handle);
        return cred;
    }

    if (pswd_blob_len > 64) {
        ESP_LOGE(TAG, "Invalid STA password data length");
        nvs_close(handle);
        return cred;
    }
    uint8_t *pswd_blob = pswd_blob_len ? (uint8_t *) malloc(pswd_blob_len) : NULL;
    if (pswd_blob_len != 0 && pswd_blob == NULL) {
        ESP_LOGE(TAG, "Failed to allocate password data");
        nvs_close(handle);
        return cred;
    }
    if (pswd_blob_len != 0) {
        err = nvs_get_blob(handle, "sta.pswd", pswd_blob, &pswd_blob_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read the sta.pswd blob data: %s", esp_err_to_name(err));
            free(pswd_blob);
            nvs_close(handle);
            return cred;
        }
    }
    bool password_valid = parse_sta_pswd_blob(pswd_blob, pswd_blob_len, cred.password);
    free(pswd_blob);

    nvs_close(handle);
    cred.is_valid = ssid_valid && password_valid;

    return cred;
}
