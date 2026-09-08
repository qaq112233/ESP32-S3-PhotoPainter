#pragma once

// Portable protocol primitives for PhotoPull.  This header deliberately has
// no ESP-IDF or FreeRTOS dependency so it can also be exercised on a host.

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace photopull {

static const size_t kConfigMaxBytes = 8 * 1024;
static const size_t kManifestMaxBytes = 32 * 1024;
static const size_t kMaxPhotos = 50;
static const size_t kMaxPhotoIdBytes = 64;
static const size_t kMaxPhotoPathBytes = 256;
static const size_t kMaxEtagBytes = 256;
static const size_t kMaxGenerationBytes = 64;
static const size_t kMaxSourceBytes = 1024;
static const uint32_t kMinIntervalSec = 60;
static const uint32_t kMaxIntervalSec = 86400;
static const uint32_t kDefaultPollIntervalSec = 60;
static const uint32_t kDefaultDisplayIntervalSec = 300;
static const uint32_t kExpectedBmpSize = 1152054;
static const char kManagedDirectory[] = "/sdcard/07_server_photos";

struct PhotoEntry {
    std::string id;
    std::string path;
    std::string sha256;  // Always lower-case after parsing.
    uint32_t size = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::string format;
};

struct Manifest {
    uint32_t schema = 0;
    std::string generation;
    uint32_t revision = 0;
    bool has_display_interval = false;
    uint32_t display_interval_sec = kDefaultDisplayIntervalSec;
    std::vector<PhotoEntry> photos;

    // cJSON's compact printer is not used here.  The parser emits a compact,
    // deterministic representation with a fixed field order, so a same-version
    // comparison is independent of JSON whitespace and object key order.
    std::string normalized_json;
};

struct PhotoPullConfig {
    uint32_t schema = 0;
    bool enabled = false;
    bool has_wifi = false;
    std::string ssid;
    std::string password;
    bool has_server = false;
    std::string base_url;
    std::string manifest_path;
    std::string bearer_token;
    std::string ca_file;
    uint32_t poll_interval_sec = kDefaultPollIntervalSec;
    uint32_t default_display_interval_sec = kDefaultDisplayIntervalSec;
};

enum class ParseStatus {
    kOk = 0,
    kEmpty,
    kTooLarge,
    kMalformed,
    kMissingField,
    kDuplicateField,
    kUnknownField,
    kInvalidValue,
};

struct ParseResult {
    ParseStatus status = ParseStatus::kMalformed;
    size_t offset = 0;
    std::string message;

    bool ok() const { return status == ParseStatus::kOk; }
};

ParseResult ParseManifestJson(const char* data, size_t length, Manifest* out,
                              uint32_t default_display_interval_sec = kDefaultDisplayIntervalSec);
ParseResult ParseConfigJson(const char* data, size_t length, PhotoPullConfig* out);

// Build the canonical source key used for revision and ETag persistence.  A
// source is an HTTPS origin followed by one validated absolute path; the
// origin is lower-cased and an explicit :443 is removed.  The path remains
// case-sensitive and is never URL-decoded.
bool NormalizeSourceUrl(const std::string& source, std::string* normalized);

// A config file is read through a tiny callback instead of stdio so the same
// parser can be used by the SD service and host tests.
typedef bool (*ReadWholeFileFn)(void* context, const char* path,
                                std::vector<uint8_t>* data);
ParseResult LoadConfig(ReadWholeFileFn read_file, void* context, const char* path,
                       PhotoPullConfig* out);

enum class VersionDecision {
    kAccept = 0,
    kNotModified,
    kRejectRevisionRollback,
    kRejectSameRevisionConflict,
};

// `same_source` is intentionally passed by the caller.  A source change resets
// ETag/revision comparison while preserving the old local library for playback.
VersionDecision CompareManifest(const Manifest& candidate, const Manifest* current,
                                bool same_source);

bool IsValidManagedSha256(const std::string& sha256);
std::string ManagedImagePath(const std::string& sha256);
std::string ManagedPartPath(const std::string& sha256);

const char* ParseStatusString(ParseStatus status);
const char* VersionDecisionString(VersionDecision decision);

}  // namespace photopull
