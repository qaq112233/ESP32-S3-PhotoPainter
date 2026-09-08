#include "service.h"
#include "bmp_validator.h"
#include "carousel.h"
#include "core.h"
#include "fatfs_store.h"
#include "https_io.h"
#include "store.h"
#include "user_app.h"
#include "power_bsp.h"
#include "button_bsp.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <new>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "mbedtls/x509_crt.h"

namespace {
using namespace photopull;
constexpr char kUploadPart[] = "/sdcard/02_sys_ap_img/photopull-upload.part";
constexpr char kUploadFile[] = "/sdcard/02_sys_ap_img/user_send.bmp";
constexpr char kUploadBackup[] = "/sdcard/02_sys_ap_img/photopull-upload.previous.bmp";
constexpr char kWebPrefix[] = "/sdcard/03_sys_ap_html/";
const char* TAG = "photopull";
int64_t Now() { return esp_timer_get_time() / 1000; }

PhotoPullConfig config;
bool config_valid = false, storage_available = false;
std::atomic<bool> active{false}, ready{false}, stopping{false}, stopped{true}, connected{false};
std::atomic<bool> startup_healthy{true};
std::atomic<bool> battery_pending{false};
// A single manual slot includes uploads and battery displays.
std::atomic<bool> manual_reserved{false};
enum class RpcKind : uint8_t { Begin, Write, Finish, Abort, Read };
struct RpcRequest {
    RpcKind kind;
    size_t size = 0, offset = 0;
    char path[257] = {};
    uint8_t data[kIoBlock] = {};
};
struct RpcReply {
    esp_err_t result = ESP_FAIL;
    size_t size = 0, total = 0;
    uint8_t data[kIoBlock] = {};
};
QueueHandle_t rpc_requests = nullptr, rpc_replies = nullptr;
SemaphoreHandle_t rpc_mutex = nullptr;
RpcRequest rpc_call_buffer;
RpcReply rpc_result_buffer;
bool rpc_timed_out = false; // protected by rpc_mutex; never reuse an outstanding response.

class Manager {
public:
    Manager() : store_(&fs_, ValidateStoredBmp) {}
    static void Entry(void* arg) { static_cast<Manager*>(arg)->Run(); }
private:
    FatFsStore fs_;
    Store store_;
    HttpsIo io_;
    Carousel carousel_;
    ImageWriter writer_;
    Manifest candidate_;
    CandidatePlan candidate_plan_;
    std::string source_, candidate_etag_, manifest_body_;
    HttpEvent event_;
    RpcRequest request_;
    RpcReply reply_;
    enum class Phase { Idle, Manifest, Image } phase_ = Phase::Idle;
    uint32_t serial_ = 0, failures_ = 0;
    size_t photo_index_ = 0;
    int http_status_ = 0;
    bool io_started_ = false, sync_allowed_ = false, upload_open_ = false, upload_display_ = false;
    bool in_maintenance_ = false, recovery_pending_ = false;
    size_t upload_bytes_ = 0;
    int64_t next_poll_ = 0, round_deadline_ = 0, operation_deadline_ = 0, mirror_retry_ = 0;
    int64_t upload_activity_ = 0, stats_at_ = 0;

    void Maintenance() {
        if (stopping || !ready || in_maintenance_) return;
        in_maintenance_ = true;
        if (xQueueReceive(rpc_requests, &request_, 0) == pdTRUE) {
            HandleRpc();
            xQueueSend(rpc_replies, &reply_, 0);
        }
        DisplayWork();
        in_maintenance_ = false;
    }

    void UpdateCarousel() {
        const auto& r = store_.recovery();
        if (!r.has_snapshot) { carousel_.unavailable(); return; } // Unknown is never an empty library.
        std::vector<Slide> slides;
        for (size_t i = 0; i < r.snapshot.manifest.photos.size(); ++i) {
            if (i < r.valid_images.size() && r.valid_images[i]) {
                const auto& p = r.snapshot.manifest.photos[i];
                slides.push_back({p.id, p.sha256, ManagedImagePath(p.sha256)});
            }
        }
        // A damaged non-empty manifest with no readable image must preserve the screen.
        if (slides.empty() && !r.snapshot.manifest.photos.empty()) { carousel_.unavailable(); return; }
        carousel_.update(std::move(slides), r.snapshot.manifest.display_interval_sec, Now());
    }
    bool LoadCa(std::string* ca) {
        if (config.ca_file.empty()) return true;
        if (config.ca_file.compare(0, 8, "/sdcard/") != 0) return false;
        std::vector<uint8_t> bytes;
        if (!fs_.ReadFile(config.ca_file, &bytes) || bytes.empty() || bytes.size() > 16384) return false;
        bytes.push_back(0);
        mbedtls_x509_crt cert;
        mbedtls_x509_crt_init(&cert);
        bool valid = mbedtls_x509_crt_parse(&cert, bytes.data(), bytes.size()) == 0;
        mbedtls_x509_crt_free(&cert);
        if (!valid) return false;
        ca->assign(reinterpret_cast<const char*>(bytes.data()), bytes.size() - 1);
        return true;
    }
    void Run() {
        if (storage_available) {
            if (mkdir(kManagedDirectory, 0775) != 0 && errno != EEXIST) storage_available = false;
            if (storage_available) {
                RecoveryState recovery;
                auto status = store_.Recover(&recovery);
                ESP_LOGI(TAG, "Library recovery: %s, complete=%d", Store::StatusString(status), recovery.complete);
                UpdateCarousel();
                // Recover the last upload if a reset occurred between preserving
                // the previous file and publishing the newly validated upload.
                bmp_validator_info_t upload_info;
                if (bmp_validator_validate_file(kUploadFile, &upload_info) != BMP_VALIDATOR_OK &&
                    bmp_validator_validate_file(kUploadBackup, &upload_info) == BMP_VALIDATOR_OK)
                    fs_.Rename(kUploadBackup, kUploadFile);
            }
        }
        fs_.SetProgress([](void* self) { static_cast<Manager*>(self)->Maintenance(); }, this);
        sync_allowed_ = storage_available && config_valid && config.enabled;
        if (sync_allowed_) {
            std::string ca;
            if (!LoadCa(&ca)) {
                sync_allowed_ = false;
                ESP_LOGE(TAG, "Private CA unavailable or invalid; synchronization disabled");
            } else {
                source_ = config.base_url + config.manifest_path;
                io_started_ = io_.start(config.base_url, config.bearer_token, ca);
                startup_healthy = io_started_;
                sync_allowed_ = io_started_;
            }
        }
        // Task allocation is part of startup confirmation. DNS, TLS and SNTP
        // start only after the local manager is ready and networking connects.
        ready = true;
        while (!stopping) {
            io_.connected(connected);
            if (recovery_pending_) {
                recovery_pending_ = false;
                RecoveryState checked;
                store_.Recover(&checked);
                UpdateCarousel();
            }
            // Every accepted maintenance operation gets service between bounded work units.
            Maintenance();
            if (stopping) break;
            if (upload_open_ && Now() - upload_activity_ > 330000) AbortUpload();
            if (phase_ != Phase::Idle && (Now() >= operation_deadline_ || Now() >= round_deadline_ || !connected)) FailSync();
            if (io_.receive(event_) && phase_ != Phase::Idle && event_.serial == serial_) HandleHttp();
            const auto& r = store_.recovery();
            if (phase_ == Phase::Idle && r.complete && r.mirror_pending && Now() >= mirror_retry_) {
                bool mirrored = false, gc = false;
                store_.RepairMirror(&mirrored, &gc);
                mirror_retry_ = Now() + 30000;
            }
            if (phase_ == Phase::Idle && sync_allowed_ && connected && Now() >= next_poll_ &&
                !(store_.recovery().complete && store_.recovery().mirror_pending)) BeginSync();
            if (Now() >= stats_at_) {
                ESP_LOGI(TAG, "resources internal=%u psram=%u largest_internal=%u largest_psram=%u manager_stack=%u https_stack=%u io_queue=%u rpc_queue=%u",
                         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                         static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                         static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)),
                         static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)), io_.stack_watermark(),
                         io_.queued_blocks(), static_cast<unsigned>(uxQueueMessagesWaiting(rpc_requests)));
                stats_at_ = Now() + 60000;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        io_.stop();
        writer_.Abort();
        AbortUpload();
        // The driver has already completed its bounded power-off path before the loop exits.
        while (io_started_ && !io_.stopped()) vTaskDelay(pdMS_TO_TICKS(20));
        stopped = true;
        vTaskDelete(nullptr);
    }
    void FailSync() {
        writer_.Abort();
        io_.cancel();
        phase_ = Phase::Idle;
        manifest_body_.clear();
        uint32_t cap = std::max<uint32_t>(config.poll_interval_sec, 3600);
        uint64_t delay = static_cast<uint64_t>(config.poll_interval_sec) << std::min<uint32_t>(failures_, 16);
        delay = std::min<uint64_t>(delay, cap);
        if (failures_ < 16) ++failures_;
        int jitter = static_cast<int>(esp_random() % 201) - 100;
        next_poll_ = Now() + static_cast<int64_t>(delay) * (1000 + jitter);
        ESP_LOGW(TAG, "Synchronization failed; committed library retained, retry in %lld s", (next_poll_ - Now()) / 1000);
    }
    bool Request(const std::string& path, const std::string& etag, bool image) {
        if (path.size() > 256 || etag.size() > 256 || config.base_url.size() + path.size() > 512) return false;
        HttpRequest request;
        request.serial = ++serial_;
        memcpy(request.path, path.c_str(), path.size() + 1);
        memcpy(request.etag, etag.c_str(), etag.size() + 1);
        request.limit = image ? kExpectedBmpSize : kManifestMaxBytes;
        request.budget_ms = image ? 300000 : 30000;
        operation_deadline_ = Now() + request.budget_ms;
        http_status_ = 0;
        return io_.submit(request);
    }
    void BeginSync() {
        if (stopping) return;
        // Revalidate before granting an ETag: 304 cannot prove local integrity.
        RecoveryState recovered;
        store_.Recover(&recovered);
        UpdateCarousel();
        if (recovered.complete && recovered.mirror_pending) { next_poll_ = Now() + 30000; return; }
        std::string etag;
        // A complete older slot may be used for offline playback while a
        // newer structurally valid slot awaits image repair.  Its ETag must
        // not authorize a 304: the newer record remains the version basis and
        // needs an unconditional manifest response.
        if (recovered.has_snapshot && recovered.complete &&
            !recovered.has_newer_incomplete && recovered.snapshot.source == source_)
            etag = recovered.snapshot.etag;
        manifest_body_.clear();
        candidate_etag_.clear();
        round_deadline_ = Now() + 30 * 60 * 1000;
        phase_ = Phase::Manifest;
        if (!Request(config.manifest_path, etag, false)) FailSync();
    }
    void CompleteSync() {
        phase_ = Phase::Idle;
        failures_ = 0;
        next_poll_ = Now() + static_cast<int64_t>(config.poll_interval_sec) * 1000;
        manifest_body_.clear();
    }
    void NextPhoto() {
        if (stopping) { writer_.Abort(); io_.cancel(); phase_ = Phase::Idle; return; }
        if (Now() >= round_deadline_) { FailSync(); return; }
        while (photo_index_ < candidate_plan_.photos.size()) {
            auto& missing = candidate_plan_.photos[photo_index_];
            if (missing.valid) { ++photo_index_; continue; }
            bool valid = false; // Multiple IDs may share one newly downloaded SHA.
            store_.CheckPhoto(missing.photo, &valid);
            if (valid) { ++photo_index_; continue; }
            if (store_.BeginImage(missing.photo, &writer_) != StoreStatus::kOk) { FailSync(); return; }
            phase_ = Phase::Image;
            if (!Request(missing.photo.path, "", true)) FailSync();
            return;
        }
        auto result = store_.Commit(candidate_, source_, candidate_etag_, true);
        if (!result.committed) { FailSync(); return; }
        UpdateCarousel();
        ESP_LOGI(TAG, "Library committed: seq=%llu photos=%u mirrored=%d", result.commit_seq,
                 static_cast<unsigned>(candidate_.photos.size()), result.mirrored);
        CompleteSync();
    }
    void HandleHttp() {
        if (event_.kind == HttpEventKind::Failed) { FailSync(); return; }
        if (event_.kind == HttpEventKind::Headers) {
            http_status_ = event_.status;
            if (phase_ == Phase::Manifest) {
                candidate_etag_ = event_.etag;
                if (http_status_ == 304 &&
                    (!store_.recovery().complete || store_.recovery().has_newer_incomplete ||
                     store_.recovery().snapshot.source != source_)) FailSync();
            } else if (http_status_ != 200 || (event_.content_length >= 0 && event_.content_length != kExpectedBmpSize)) FailSync();
            return;
        }
        if (event_.kind == HttpEventKind::Data) {
            if (http_status_ != 200) { FailSync(); return; }
            if (phase_ == Phase::Manifest) {
                if (manifest_body_.size() + event_.size > kManifestMaxBytes) { FailSync(); return; }
                manifest_body_.append(reinterpret_cast<char*>(event_.data), event_.size);
            } else if (writer_.Write(event_.data, event_.size) != StoreStatus::kOk) FailSync();
            return;
        }
        if (event_.kind != HttpEventKind::Complete) return;
        if (phase_ == Phase::Image) {
            if (http_status_ != 200 || writer_.Finish() != StoreStatus::kOk) { FailSync(); return; }
            ++photo_index_;
            NextPhoto();
            return;
        }
        if (http_status_ == 304) {
            RecoveryState checked;
            store_.Recover(&checked);
            UpdateCarousel();
            if (!checked.complete || checked.has_newer_incomplete || checked.snapshot.source != source_) {
                FailSync(); return;
            }
            CompleteSync(); return;
        }
        if (http_status_ != 200 || !ParseManifestJson(manifest_body_.data(), manifest_body_.size(), &candidate_, config.default_display_interval_sec).ok()) {
            FailSync(); return;
        }
        auto status = store_.PlanCandidate(candidate_, source_, &candidate_plan_);
        if (status != StoreStatus::kOk) { FailSync(); return; }
        uint64_t total, available;
        // Retain all current data and enough space for snapshots and one manual upload.
        if (esp_vfs_fat_info("/sdcard", &total, &available) != ESP_OK ||
            available < candidate_plan_.total_missing_bytes + kExpectedBmpSize + 256 * 1024) { FailSync(); return; }
        photo_index_ = 0;
        NextPhoto();
    }
    esp_err_t ShowBattery() {
        PmicRegisterConfig p = Custom_PmicGetBatteryInfo();
        esp_err_t status = ePaperDisplay.EPD_DispClear(ColorWhite);
        if (status == ESP_OK) status = ePaperDisplay.EPD_DrawStringEN(120, 180, p.isCharging, &Font24, ColorWhite, ColorBlack);
        if (status == ESP_OK) status = ePaperDisplay.EPD_DrawStringEN(120, 220, p.chargeStatus, &Font24, ColorWhite, ColorBlack);
        if (status == ESP_OK) status = ePaperDisplay.EPD_DrawStringEN(120, 260, p.batteryVoltage, &Font24, ColorWhite, ColorBlack);
        if (status == ESP_OK) status = ePaperDisplay.EPD_DrawStringEN(120, 300, p.batteryPercent, &Font24, ColorWhite, ColorBlack);
        return status == ESP_OK ? ePaperDisplay.EPD_Display() : status;
    }
    void DisplayWork() {
        if (!carousel_.retry_ready(Now())) return;
        auto intent = carousel_.due(Now());
        bool manual = !carousel_.urgent_clear() && (upload_display_ || battery_pending);
        if (!manual && !intent.available) return;
        if (!manual && !intent.clear && intent.physical) {
            bool valid = false;
            for (const auto& photo : store_.recovery().snapshot.manifest.photos) {
                if (photo.id == intent.slide.id) { store_.CheckPhoto(photo, &valid); break; }
            }
            if (!valid) {
                // Offline damage changes only the playable mask. Never commit
                // the shortened list or treat it as an authoritative empty one.
                // Maintenance may run from a Store validation progress callback.
                // Defer mutation of recovery_ until that outer Store operation
                // returns, preserving its references and commit decisions.
                recovery_pending_ = true;
                carousel_.failed(Now());
                next_poll_ = 0;
                return;
            }
        }
        if (xSemaphoreTake(epaper_gui_semapHandle, pdMS_TO_TICKS(10)) != pdTRUE) return;
        // No SD/HTTP work occurs while the panel transaction owns the manager.
        Green_led_arg = 1;
        xEventGroupSetBits(Green_led_Mode_queue, set_bit_button(6));
        esp_err_t status;
        if (manual) status = upload_display_ ? ePaperDisplay.EPD_ShowBmp(kUploadFile) : ShowBattery();
        else if (!intent.physical) status = ESP_OK;
        else status = intent.clear ? ePaperDisplay.EPD_ShowClear(ColorWhite) : ePaperDisplay.EPD_ShowBmp(intent.slide.path.c_str());
        Green_led_arg = 0;
        xSemaphoreGive(epaper_gui_semapHandle);
        if (status != ESP_OK) {
            ESP_LOGE(TAG, "Display transaction failed (%s); retry delayed", esp_err_to_name(status));
            carousel_.failed(Now());
            return;
        }
        if (manual) {
            upload_display_ = false;
            battery_pending = false;
            manual_reserved = false;
            carousel_.manual_displayed(Now());
        } else carousel_.displayed(intent, Now());
    }
    void AbortUpload() {
        if (upload_open_) fs_.Remove(kUploadPart);
        upload_open_ = false;
        upload_bytes_ = 0;
    }
    void HandleRpc() {
        reply_.result = ESP_FAIL;
        reply_.size = reply_.total = 0;
        if (!storage_available) { reply_.result = ESP_ERR_NOT_FOUND; return; }
        switch (request_.kind) {
        case RpcKind::Begin: {
            if (upload_open_ || manual_reserved) { reply_.result = ESP_ERR_INVALID_STATE; break; }
            if (request_.size != kExpectedBmpSize) { reply_.result = ESP_ERR_INVALID_SIZE; break; }
            if (mkdir("/sdcard/02_sys_ap_img", 0775) != 0 && errno != EEXIST) break;
            uint64_t total, free;
            if (esp_vfs_fat_info("/sdcard", &total, &free) != ESP_OK || free < kExpectedBmpSize + 65536) break;
            size_t written;
            if (!fs_.WriteFile(kUploadPart, nullptr, 0, false, &written)) break;
            upload_open_ = true;
            upload_bytes_ = 0;
            upload_activity_ = Now();
            reply_.result = ESP_OK;
            break;
        }
        case RpcKind::Write: {
            if (!upload_open_) { reply_.result = ESP_ERR_INVALID_STATE; break; }
            if (request_.size > kIoBlock || upload_bytes_ + request_.size > kExpectedBmpSize) {
                AbortUpload(); reply_.result = ESP_ERR_INVALID_SIZE; break;
            }
            size_t written;
            if (!fs_.WriteFile(kUploadPart, request_.data, request_.size, true, &written) || written != request_.size) { AbortUpload(); break; }
            upload_bytes_ += request_.size;
            upload_activity_ = Now();
            reply_.result = ESP_OK;
            break;
        }
        case RpcKind::Finish: {
            if (!upload_open_) { reply_.result = ESP_ERR_INVALID_STATE; break; }
            bool expected = false;
            if (!manual_reserved.compare_exchange_strong(expected, true)) { AbortUpload(); reply_.result = ESP_ERR_INVALID_STATE; break; }
            bmp_validator_info_t info;
            bool valid = upload_bytes_ == kExpectedBmpSize && fs_.SyncFile(kUploadPart) &&
                         bmp_validator_validate_file(kUploadPart, &info) == BMP_VALIDATOR_OK;
            if (valid) {
                size_t size;
                bool old_exists = fs_.StatFile(kUploadFile, &size);
                if (old_exists) valid = fs_.Remove(kUploadBackup) && fs_.Rename(kUploadFile, kUploadBackup);
                if (valid && !fs_.Rename(kUploadPart, kUploadFile)) {
                    if (old_exists) fs_.Rename(kUploadBackup, kUploadFile);
                    valid = false;
                }
            }
            if (!valid) { manual_reserved = false; AbortUpload(); break; }
            upload_open_ = false;
            upload_display_ = true;
            reply_.result = ESP_OK;
            break;
        }
        case RpcKind::Abort: AbortUpload(); reply_.result = ESP_OK; break;
        case RpcKind::Read: {
            std::string path = request_.path;
            if (path.compare(0, strlen(kWebPrefix), kWebPrefix) != 0 || path.find("..") != std::string::npos ||
                path.find_first_of("%\\?#") != std::string::npos || request_.size > kIoBlock) { reply_.result = ESP_ERR_INVALID_ARG; break; }
            if (!fs_.StatFile(path, &reply_.total)) { reply_.result = ESP_ERR_NOT_FOUND; break; }
            if (request_.offset > reply_.total) { reply_.result = ESP_ERR_INVALID_SIZE; break; }
            FILE* file = fopen(path.c_str(), "rb");
            if (!file) break;
            bool ok = fseek(file, request_.offset, SEEK_SET) == 0;
            size_t count = std::min(request_.size, reply_.total - request_.offset);
            if (ok) { reply_.size = fread(reply_.data, 1, count, file); ok = reply_.size == count && !ferror(file); }
            if (fclose(file) != 0) ok = false;
            reply_.result = ok ? ESP_OK : ESP_FAIL;
            break;
        }
        }
    }
};
Manager* manager = nullptr;

esp_err_t Rpc(RpcKind kind, const uint8_t* data, size_t size, const char* path = nullptr, size_t offset = 0,
              uint8_t* out = nullptr, size_t* read = nullptr, size_t* total = nullptr) {
    if (!active || !ready || stopping || !rpc_mutex) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(rpc_mutex, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;
    esp_err_t result = ESP_ERR_TIMEOUT;
    do {
        if (rpc_timed_out) {
            if (xQueueReceive(rpc_replies, &rpc_result_buffer, 0) != pdTRUE) break;
            rpc_timed_out = false;
        }
        rpc_call_buffer.kind = kind;
        rpc_call_buffer.size = size;
        rpc_call_buffer.offset = offset;
        rpc_call_buffer.path[0] = 0;
        if (path) {
            if (strlen(path) > 256) { result = ESP_ERR_INVALID_ARG; break; }
            strcpy(rpc_call_buffer.path, path);
        }
        if (data) {
            if (size > kIoBlock) { result = ESP_ERR_INVALID_SIZE; break; }
            memcpy(rpc_call_buffer.data, data, size);
        }
        if (xQueueSend(rpc_requests, &rpc_call_buffer, 0) != pdTRUE) break;
        if (xQueueReceive(rpc_replies, &rpc_result_buffer, pdMS_TO_TICKS(160000)) != pdTRUE) {
            rpc_timed_out = true;
            break;
        }
        result = rpc_result_buffer.result;
        if (result == ESP_OK) {
            if (read) *read = rpc_result_buffer.size;
            if (total) *total = rpc_result_buffer.total;
            if (out && rpc_result_buffer.size) memcpy(out, rpc_result_buffer.data, rpc_result_buffer.size);
        }
    } while (false);
    xSemaphoreGive(rpc_mutex);
    return result;
}
}

void photopull_load_config(bool available) {
    if (active) return;
    storage_available = available;
    config_valid = false;
    config = PhotoPullConfig{};
    if (!available) return;
    FatFsStore fs;
    std::vector<uint8_t> bytes;
    size_t length;
    if (!fs.StatFile("/sdcard/photopull.json", &length) || length > kConfigMaxBytes || !fs.ReadFile("/sdcard/photopull.json", &bytes)) return;
    auto parsed = ParseConfigJson(reinterpret_cast<char*>(bytes.data()), bytes.size(), &config);
    config_valid = parsed.ok();
    if (!config_valid) ESP_LOGW(TAG, "Configuration invalid (%s); remote synchronization disabled", ParseStatusString(parsed.status));
}
bool photopull_default_network() { return config_valid && config.enabled; }
bool photopull_wifi_credentials(char* ssid, size_t ssid_capacity, char* password, size_t password_capacity) {
    if (!config_valid || config.ssid.empty() || !ssid || !password || ssid_capacity <= config.ssid.size() || password_capacity <= config.password.size()) return false;
    memcpy(ssid, config.ssid.c_str(), config.ssid.size() + 1);
    memcpy(password, config.password.c_str(), config.password.size() + 1);
    return true;
}
bool photopull_start() {
    if (active) return !stopping;
    const auto release_failed_start = [] {
        if (rpc_requests) vQueueDelete(rpc_requests);
        if (rpc_replies) vQueueDelete(rpc_replies);
        if (rpc_mutex) vSemaphoreDelete(rpc_mutex);
        rpc_requests = rpc_replies = nullptr;
        rpc_mutex = nullptr;
        delete manager;
        manager = nullptr;
    };
    rpc_requests = xQueueCreate(1, sizeof(RpcRequest));
    rpc_replies = xQueueCreate(1, sizeof(RpcReply));
    rpc_mutex = xSemaphoreCreateMutex();
    manager = new (std::nothrow) Manager;
    if (!rpc_requests || !rpc_replies || !rpc_mutex || !manager) {
        release_failed_start();
        return false;
    }
    active = true;
    stopped = false;
    if (xTaskCreate(Manager::Entry, "photopull_manager", 16384, manager, 3, nullptr) != pdPASS) {
        active = false;
        stopped = true;
        release_failed_start();
        return false;
    }
    return true;
}
bool photopull_ready() { return ready && startup_healthy; }
bool photopull_active() { return active; }
bool photopull_stop(uint32_t timeout_ms) {
    if (!active) return true;
    stopping = true;
    int64_t deadline = Now() + timeout_ms;
    while (!stopped && Now() < deadline) vTaskDelay(pdMS_TO_TICKS(20));
    return stopped;
}
void photopull_network_changed(bool value) { connected = value; }
bool photopull_request_battery() {
    if (!active || stopping) return false;
    bool expected = false;
    if (!manual_reserved.compare_exchange_strong(expected, true)) return false;
    battery_pending = true;
    return true;
}
esp_err_t photopull_upload_begin(size_t size) { return Rpc(RpcKind::Begin, nullptr, size); }
esp_err_t photopull_upload_write(const uint8_t* data, size_t size) {
    if (!data || !size || size > kIoBlock) return ESP_ERR_INVALID_SIZE;
    return Rpc(RpcKind::Write, data, size);
}
esp_err_t photopull_upload_finish() { return Rpc(RpcKind::Finish, nullptr, 0); }
void photopull_upload_abort() { Rpc(RpcKind::Abort, nullptr, 0); }
esp_err_t photopull_web_read(const char* path, size_t offset, uint8_t* out, size_t capacity, size_t* read, size_t* total) {
    if (!path || !out || !read || !total || capacity > kIoBlock) return ESP_ERR_INVALID_ARG;
    *read = *total = 0;
    return Rpc(RpcKind::Read, nullptr, capacity, path, offset, out, read, total);
}
