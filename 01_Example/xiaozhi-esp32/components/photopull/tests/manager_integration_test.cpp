// Compile the production manager and display buffer logic. ESP APIs are
// deterministic boundary stubs; these tests do not claim hardware validation.
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <new>
#include <string>
#include <utility>
#include <vector>
#define private public
#include "../service.cpp"
#undef private

ImgDecodeDither::ImgDecodeDither() {}
ImgDecodeDither::~ImgDecodeDither() {}
ImgDecodeDither test_dither;
ePaperPort ePaperDisplay(test_dither, 1, 2, 3, 4, 5, 6, 800, 480, 4096, 4096);
std::atomic<uint8_t> Green_led_arg{0};
SemaphoreHandle_t epaper_gui_semapHandle = reinterpret_cast<void*>(1);
HostEventGroup green_events;
EventGroupHandle_t Green_led_Mode_queue = &green_events;
namespace photopull {
bool HttpsIo::start(const std::string&, const std::string&, const std::string&) { return true; }
bool HttpsIo::submit(const HttpRequest&) { return true; }
bool HttpsIo::receive(HttpEvent&) { return false; }
void HttpsIo::cancel() {}
void HttpsIo::stop() { stopped_ = true; }
}

class MemoryFs final : public photopull::FileSystem {
public:
    std::map<std::string, std::vector<uint8_t>> files;
    bool fail_remove = false;
    unsigned remove_calls = 0;
    bool ReadFile(const std::string& path, std::vector<uint8_t>* data) override {
        const auto found = files.find(path);
        if (found == files.end()) return false;
        *data = found->second; return true;
    }
    bool ReadFileChunks(const std::string& path, ChunkCallback callback, void* ctx) override {
        const auto found = files.find(path);
        if (found == files.end()) return false;
        return callback(found->second.data(), found->second.size(), ctx);
    }
    bool WriteFile(const std::string& path, const uint8_t* data, size_t len, bool append, size_t* written) override {
        auto& target = files[path];
        if (!append) target.clear();
        if (len) target.insert(target.end(), data, data + len);
        *written = len; return true;
    }
    bool SyncFile(const std::string&) override { return true; }
    bool SyncDirectory(const std::string&) override { return true; }
    bool Rename(const std::string& from, const std::string& to) override {
        if (!files.count(from)) return false;
        files[to] = std::move(files[from]); files.erase(from); return true;
    }
    bool Remove(const std::string& path) override {
        ++remove_calls;
        if (fail_remove) return false;
        files.erase(path); return true;
    }
    bool StatFile(const std::string& path, size_t* size) override {
        const auto found = files.find(path);
        if (found == files.end()) return false;
        *size = found->second.size(); return true;
    }
    bool ListFiles(const std::string&, std::vector<std::string>* paths) override {
        paths->clear(); for (const auto& file : files) paths->push_back(file.first); return true;
    }
    uint64_t Used() const { uint64_t n = 0; for (const auto& f : files) n += f.second.size(); return n; }
};

constexpr const char* kSource = "https://example.com/manifest.json";
constexpr const char* kZero = "a4c428ff67ad2eb1d67c40cee495a0a5d613ac234657a9fa778d3ee740721aca";
constexpr const char* kOne = "e496791c49fa08b267021e8e5ddb336bd4a644fbb399de4e8e6931fe5c071729";
constexpr const char* kOld = "107ffc794338d9c5ea0eb6cc244c71f4b4080a74f38628159939cd1cb072c0d9";
bool AcceptFixture(FileSystem*, const std::string&, const PhotoEntry&, void*) { return true; }

Manifest ManifestWith(std::initializer_list<const char*> shas, unsigned revision = 1) {
    std::string json = "{\"schema\":1,\"generation\":\"g\",\"revision\":" + std::to_string(revision) + ",\"photos\":[";
    unsigned i = 0;
    for (const char* sha : shas) {
        if (i) json += ',';
        json += "{\"id\":\"p" + std::to_string(i++) + "\",\"path\":\"/p.bmp\",\"sha256\":\"" + sha +
            "\",\"size\":1152054,\"width\":800,\"height\":480,\"format\":\"bmp24-6color\"}";
    }
    json += "]}";
    Manifest parsed; assert(ParseManifestJson(json.data(), json.size(), &parsed).ok()); return parsed;
}
void Initialize(Manager& manager, MemoryFs& fs) {
    host_now_us = 0; host_delay_hook = nullptr; host_free_space = {};
    config = PhotoPullConfig{}; config.base_url = "https://example.com";
    ready = true; stopping = false; storage_available = false;
    battery_pending = false; manual_reserved = false; connected = true;
    manager.store_.fs_ = &fs;
    manager.store_.validator_ = AcceptFixture;
    manager.source_ = kSource;
    assert(manager.store_.Commit(ManifestWith({}), kSource, "etag").mirrored);
}
void Recover(Manager& m) {
    RecoveryState recovered;
    assert(m.store_.Recover(&recovered) == StoreStatus::kOk);
    assert(recovered.complete && !recovered.mirror_pending && recovered.gc_pending);
}

void TestCleanupBeforeSpaceCheckKeepsCandidate() {
    MemoryFs fs; Manager m; Initialize(m, fs);
    fs.files[ManagedImagePath(kOld)] = std::vector<uint8_t>(kExpectedBmpSize, 255);
    fs.files[ManagedImagePath(kZero)] = std::vector<uint8_t>(kExpectedBmpSize, 0);
    Recover(m);
    m.CleanupPending(); // Before the first server response, preserve retryable downloads.
    assert(fs.files.count(ManagedImagePath(kOld)) && fs.files.count(ManagedImagePath(kZero)));
    const uint64_t capacity = fs.Used() + 2000000;
    host_free_space = [&] { return capacity - fs.Used(); };
    m.phase_ = Manager::Phase::Manifest;
    m.http_status_ = 200;
    m.event_.kind = HttpEventKind::Complete;
    m.manifest_body_ = ManifestWith({kZero, kOne}, 2).normalized_json;
    m.round_deadline_ = 1800000;
    m.HandleHttp();
    assert(m.phase_ == Manager::Phase::Image); // Preflight succeeds only after reclamation.
    assert(!fs.files.count(ManagedImagePath(kOld)));
    assert(fs.files.count(ManagedImagePath(kZero)));
    assert(m.photo_index_ == 1 && m.writer_.open()); // Already downloaded SHA is reused.
    const uint8_t first_block[] = {1, 1, 1, 1};
    assert(m.writer_.Write(first_block, sizeof(first_block)) == StoreStatus::kOk);
    m.store_.recovery_.gc_pending = true;
    host_now_us = 31000000;
    const auto removals = fs.remove_calls;
    m.CleanupPending();
    assert(fs.remove_calls == removals && fs.files.count(ManagedPartPath(kOne)));
    m.FailSync();
    m.phase_ = Manager::Phase::Manifest; m.http_status_ = 200;
    m.manifest_body_ = ManifestWith({kOld}, 0).normalized_json;
    m.HandleHttp(); // A rejected rollback must not replace the retained candidate.
    assert(m.phase_ == Manager::Phase::Idle && m.candidate_.revision == 2);
    m.CleanupPending();
    assert(fs.files.count(ManagedImagePath(kZero)));
    host_free_space = {};
}

void TestCleanupRetryAnd304ManagerLoop() {
    MemoryFs fs; Manager m; Initialize(m, fs);
    const auto orphan = ManagedImagePath(kOld);
    fs.files[orphan] = {42}; Recover(m);
    m.phase_ = Manager::Phase::Manifest; m.http_status_ = 304;
    m.event_.kind = HttpEventKind::Complete;
    m.HandleHttp();
    assert(m.candidate_known_ && m.phase_ == Manager::Phase::Idle);
    fs.fail_remove = true;
    m.CleanupPending();
    assert(m.store_.recovery().gc_pending);
    const unsigned calls = fs.remove_calls;
    host_now_us = 29999000; m.CleanupPending();
    assert(fs.remove_calls == calls);
    fs.fail_remove = false; host_now_us = 30000000;
    // Run one real manager iteration, proving the idle loop schedules cleanup.
    host_delay_hook = [](TickType_t n) { if (n == 10) stopping = true; };
    m.Run(); host_delay_hook = nullptr;
    assert(!fs.files.count(orphan) && !m.store_.recovery().gc_pending);
}

std::string WriteBmp(unsigned width, unsigned height) {
    std::vector<uint8_t> bmp(kExpectedBmpSize, 255);
    std::fill(bmp.begin(), bmp.begin() + 54, 0);
    auto u32 = [&](unsigned offset, unsigned value) {
        for (unsigned i = 0; i < 4; ++i) bmp[offset + i] = static_cast<uint8_t>(value >> (8 * i));
    };
    bmp[0] = 'B'; bmp[1] = 'M'; u32(2, bmp.size()); u32(10, 54); u32(14, 40);
    u32(18, width); u32(22, height); bmp[26] = 1; bmp[28] = 24; u32(34, bmp.size() - 54);
    char path[] = "/tmp/photopull-status-XXXXXX";
    const int fd = mkstemp(path); assert(fd >= 0);
    FILE* file = fdopen(fd, "wb"); assert(file);
    assert(fwrite(bmp.data(), 1, bmp.size(), file) == bmp.size()); assert(fclose(file) == 0);
    return path;
}
void TestBatteryLayoutDoesNotInheritPhoto() {
    Manager m;
    const auto landscape = WriteBmp(800, 480), portrait = WriteBmp(480, 800);
    assert(ePaperDisplay.EPD_SDcardBmpShakingColor(landscape.c_str(), 0, 0) == ESP_OK);
    assert(m.ShowBattery() == ESP_OK);
    const std::vector<uint8_t> expected(ePaperDisplay.RotationBuffer, ePaperDisplay.RotationBuffer + ePaperDisplay.DisplayLen);
    assert(ePaperDisplay.EPD_SDcardBmpShakingColor(portrait.c_str(), 0, 0) == ESP_OK);
    assert(ePaperDisplay.Rotation == 3);
    assert(m.ShowBattery() == ESP_OK);
    assert(ePaperDisplay.Rotation == 2 && ePaperDisplay.src_width == 800 && ePaperDisplay.src_height == 480);
    assert(std::equal(expected.begin(), expected.end(), ePaperDisplay.RotationBuffer));
    assert(unlink(landscape.c_str()) == 0 && unlink(portrait.c_str()) == 0);
}
int main() {
    TestCleanupBeforeSpaceCheckKeepsCandidate();
    TestCleanupRetryAnd304ManagerLoop();
    TestBatteryLayoutDoesNotInheritPhoto();
    puts("manager cleanup, capacity, retry reuse and battery layout passed");
}
