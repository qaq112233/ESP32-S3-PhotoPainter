#pragma once

/*
 * Dependency-free HTTP protocol rules shared by the firmware adapter and its
 * host tests.  Keep these rules separate from esp_http_server types so the
 * request boundary can be checked without an ESP-IDF build.
 */
#include <stddef.h>
#include <string.h>

namespace photopainter_server {

static constexpr size_t kWebReadMax = 4096;
static constexpr size_t kUploadBmpMax = 2U * 1024U * 1024U;

inline bool IsStaticUri(const char* uri) {
    return uri != nullptr &&
           (!strcmp(uri, "/") || !strcmp(uri, "/index.html") ||
            !strcmp(uri, "/bootstrap.min.css") ||
            !strcmp(uri, "/styles.min.css") ||
            !strcmp(uri, "/placeholder.svg") ||
            !strcmp(uri, "/bootstrap.min.js") ||
            !strcmp(uri, "/script.min.js"));
}

inline const char* StaticStoragePath(const char* uri) {
    if (uri == nullptr) return nullptr;
    if (!strcmp(uri, "/index.html")) return "/sdcard/03_sys_ap_html/index.html";
    if (!strcmp(uri, "/bootstrap.min.css")) return "/sdcard/03_sys_ap_html/bootstrap.min.css";
    if (!strcmp(uri, "/styles.min.css")) return "/sdcard/03_sys_ap_html/styles.min.css";
    if (!strcmp(uri, "/placeholder.svg")) return "/sdcard/03_sys_ap_html/placeholder.svg";
    if (!strcmp(uri, "/bootstrap.min.js")) return "/sdcard/03_sys_ap_html/bootstrap.min.js";
    if (!strcmp(uri, "/script.min.js")) return "/sdcard/03_sys_ap_html/script.min.js";
    return nullptr;
}

inline bool IsUploadContentLengthValid(size_t content_length) {
    /* The first byte selects the legacy network mode; the rest is the BMP. */
    return content_length >= 2 && content_length - 1 <= kUploadBmpMax;
}

class UploadFraming {
public:
    explicit UploadFraming(size_t content_length)
        : remaining_(content_length), first_chunk_(true) {}

    bool first_chunk() const { return first_chunk_; }
    size_t remaining() const { return remaining_; }

    /* Consume one socket chunk and return the payload portion after the
     * optional one-byte legacy header.  No arithmetic may underflow when a
     * socket splits that header into its own chunk. */
    bool Consume(size_t received, size_t* payload_offset, size_t* payload_size) {
        if (payload_offset == nullptr || payload_size == nullptr || received == 0 ||
            received > remaining_) {
            return false;
        }
        *payload_offset = first_chunk_ ? 1 : 0;
        if (first_chunk_ && received < 1) return false;
        *payload_size = received - *payload_offset;
        remaining_ -= received;
        first_chunk_ = false;
        return true;
    }

private:
    size_t remaining_;
    bool first_chunk_;
};

}  // namespace photopainter_server
