#include "core.h"

#include <ctype.h>
#include <limits.h>

#include <algorithm>
#include <stdio.h>
#include <string.h>

namespace photopull {
namespace {

class JsonReader {
public:
    JsonReader(const char* data, size_t length) : begin_(data), p_(data), end_(data + length) {}

    size_t offset() const { return static_cast<size_t>(p_ - begin_); }
    bool at_end() const { return p_ == end_; }

    void SkipWhitespace() {
        while (p_ != end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\r' || *p_ == '\n')) {
            ++p_;
        }
    }

    bool Consume(char c) {
        SkipWhitespace();
        if (p_ == end_ || *p_ != c) return false;
        ++p_;
        return true;
    }

    bool NextIs(char c) {
        SkipWhitespace();
        return p_ != end_ && *p_ == c;
    }

    bool ParseString(std::string* out) {
        SkipWhitespace();
        if (p_ == end_ || *p_ != '"') return false;
        ++p_;
        out->clear();
        while (p_ != end_) {
            const unsigned char c = static_cast<unsigned char>(*p_++);
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c != '\\') {
                out->push_back(static_cast<char>(c));
                continue;
            }
            if (p_ == end_) return false;
            const char escaped = *p_++;
            switch (escaped) {
                case '"': out->push_back('"'); break;
                case '\\': out->push_back('\\'); break;
                case '/': out->push_back('/'); break;
                case 'b': out->push_back('\b'); break;
                case 'f': out->push_back('\f'); break;
                case 'n': out->push_back('\n'); break;
                case 'r': out->push_back('\r'); break;
                case 't': out->push_back('\t'); break;
                case 'u': {
                    uint32_t codepoint = 0;
                    if (!ParseHex16(&codepoint)) return false;
                    if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
                        // JSON uses UTF-16 escape pairs for non-BMP code points.
                        if (p_ == end_ || *p_++ != '\\' || p_ == end_ || *p_++ != 'u') {
                            return false;
                        }
                        uint32_t low = 0;
                        if (!ParseHex16(&low) || low < 0xDC00 || low > 0xDFFF) return false;
                        codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                    } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
                        return false;
                    }
                    AppendUtf8(codepoint, out);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    bool ParseUint32(uint32_t* out) {
        SkipWhitespace();
        if (p_ == end_ || *p_ < '0' || *p_ > '9') return false;
        if (*p_ == '0') {
            ++p_;
            if (p_ != end_ && *p_ >= '0' && *p_ <= '9') return false;
            *out = 0;
            return true;
        }
        uint64_t value = 0;
        while (p_ != end_ && *p_ >= '0' && *p_ <= '9') {
            value = value * 10 + static_cast<unsigned>(*p_ - '0');
            if (value > UINT32_MAX) return false;
            ++p_;
        }
        *out = static_cast<uint32_t>(value);
        return true;
    }

    bool ParseBool(bool* out) {
        SkipWhitespace();
        if (Remaining("true")) {
            p_ += 4;
            *out = true;
            return true;
        }
        if (Remaining("false")) {
            p_ += 5;
            *out = false;
            return true;
        }
        return false;
    }

    bool EndValue() {
        SkipWhitespace();
        return p_ == end_;
    }

private:
    bool Remaining(const char* text) const {
        const size_t length = strlen(text);
        return static_cast<size_t>(end_ - p_) >= length && strncmp(p_, text, length) == 0;
    }

    bool ParseHex16(uint32_t* out) {
        if (static_cast<size_t>(end_ - p_) < 4) return false;
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = *p_++;
            value <<= 4;
            if (c >= '0' && c <= '9') value |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<uint32_t>(c - 'A' + 10);
            else return false;
        }
        *out = value;
        return true;
    }

    static void AppendUtf8(uint32_t cp, std::string* out) {
        if (cp <= 0x7F) {
            out->push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FF) {
            out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    const char* begin_;
    const char* p_;
    const char* end_;
};

ParseResult Error(ParseStatus status, const JsonReader& reader, const char* message) {
    ParseResult result;
    result.status = status;
    result.offset = reader.offset();
    result.message = message;
    return result;
}

ParseResult Error(ParseStatus status, size_t offset, const char* message) {
    ParseResult result;
    result.status = status;
    result.offset = offset;
    result.message = message;
    return result;
}

ParseResult Ok() {
    ParseResult result;
    result.status = ParseStatus::kOk;
    return result;
}

bool IsHex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

bool IsAsciiPathChar(char c) {
    if (c < 0x21 || c > 0x7E) return false;
    return isalnum(static_cast<unsigned char>(c)) || c == '/' || c == '.' ||
           c == '-' || c == '_' || c == '~';
}

bool ValidatePath(const std::string& path) {
    if (path.empty() || path.size() > kMaxPhotoPathBytes || path[0] != '/') return false;
    if (path.size() > 1 && path[1] == '/') return false;
    size_t component_start = 1;
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i < path.size() && path[i] != '/') {
            if (!IsAsciiPathChar(path[i])) return false;
            continue;
        }
        if (i == component_start) return false;
        const std::string component = path.substr(component_start, i - component_start);
        if (component == "." || component == "..") return false;
        component_start = i + 1;
    }
    return path.size() > 1 && path.back() != '/';
}

bool ValidateManifestString(const std::string& value, size_t max_bytes) {
    if (value.empty() || value.size() > max_bytes) return false;
    for (size_t i = 0; i < value.size(); ++i) {
        if (static_cast<unsigned char>(value[i]) < 0x20 ||
            static_cast<unsigned char>(value[i]) == 0x7f) return false;
    }
    return true;
}

bool IsHostNameChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '.' || c == '-';
}

bool ParsePort(const std::string& text, uint32_t* port) {
    if (text.empty()) return false;
    uint64_t value = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        value = value * 10 + static_cast<unsigned>(text[i] - '0');
        if (value > 65535) return false;
    }
    if (value == 0) return false;
    *port = static_cast<uint32_t>(value);
    return true;
}

bool NormalizeHttpsOrigin(const std::string& input, std::string* output) {
    if (output == nullptr || input.size() < 9 || input.size() > 512 ||
        input.compare(0, 8, "https://") != 0 || input.find('%') != std::string::npos ||
        input.find_first_of("?#\\\r\n\t ") != std::string::npos) return false;
    for (size_t i = 0; i < input.size(); ++i) {
        if (static_cast<unsigned char>(input[i]) < 0x21 || static_cast<unsigned char>(input[i]) > 0x7e) return false;
    }
    std::string authority = input.substr(8);
    if (!authority.empty() && authority.back() == '/') {
        authority.pop_back();
        if (!authority.empty() && authority.back() == '/') return false;
    }
    if (authority.empty() || authority.find('/') != std::string::npos || authority.find('@') != std::string::npos) return false;
    std::string host;
    uint32_t port = 443;
    if (authority[0] == '[') {
        const size_t close = authority.find(']');
        if (close == std::string::npos || close == 1) return false;
        if (authority.substr(1, close - 1).find(':') == std::string::npos) return false;
        host = authority.substr(0, close + 1);
        for (size_t i = 1; i < close; ++i) {
            const char c = authority[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F') || c == ':' || c == '.')) return false;
        }
        const std::string rest = authority.substr(close + 1);
        if (!rest.empty()) {
            if (rest[0] != ':' || !ParsePort(rest.substr(1), &port)) return false;
        }
    } else {
        const size_t colon = authority.find(':');
        const std::string host_part = colon == std::string::npos ? authority : authority.substr(0, colon);
        if (host_part.empty() || host_part.front() == '.' || host_part.back() == '.' ||
            host_part.front() == '-' || host_part.back() == '-') return false;
        if (host_part.find("..") != std::string::npos) return false;
        for (size_t i = 0; i < host_part.size(); ++i) if (!IsHostNameChar(host_part[i])) return false;
        host = host_part;
        if (colon != std::string::npos && !ParsePort(authority.substr(colon + 1), &port)) return false;
    }
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) {
        return static_cast<char>(tolower(c));
    });
    *output = "https://" + host;
    if (port != 443) *output += ":" + std::to_string(port);
    return true;
}

bool NormalizeSourceUrlInternal(const std::string& source, std::string* normalized) {
    if (normalized == nullptr || source.size() > kMaxSourceBytes ||
        source.compare(0, 8, "https://") != 0) return false;
    const size_t path_start = source.find('/', 8);
    if (path_start == std::string::npos || path_start == source.size()) return false;
    const std::string origin = source.substr(0, path_start);
    const std::string path = source.substr(path_start);
    std::string normalized_origin;
    if (!NormalizeHttpsOrigin(origin, &normalized_origin) || !ValidatePath(path)) return false;
    if (normalized_origin.size() > kMaxSourceBytes - path.size()) return false;
    *normalized = normalized_origin + path;
    return normalized->size() <= kMaxSourceBytes;
}

void AppendJsonString(const std::string& value, std::string* out) {
    out->push_back('"');
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < value.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        switch (c) {
            case '"': out->append("\\\""); break;
            case '\\': out->append("\\\\"); break;
            case '\b': out->append("\\b"); break;
            case '\f': out->append("\\f"); break;
            case '\n': out->append("\\n"); break;
            case '\r': out->append("\\r"); break;
            case '\t': out->append("\\t"); break;
            default:
                if (c < 0x20) {
                    out->append("\\u00");
                    out->push_back(hex[c >> 4]);
                    out->push_back(hex[c & 0x0F]);
                } else {
                    out->push_back(static_cast<char>(c));
                }
        }
    }
    out->push_back('"');
}

void AppendKey(const char* key, std::string* out) {
    AppendJsonString(key, out);
    out->push_back(':');
}

void AppendPhoto(const PhotoEntry& photo, std::string* out) {
    out->push_back('{');
    AppendKey("id", out); AppendJsonString(photo.id, out); out->push_back(',');
    AppendKey("path", out); AppendJsonString(photo.path, out); out->push_back(',');
    AppendKey("size", out); out->append(std::to_string(photo.size)); out->push_back(',');
    AppendKey("sha256", out); AppendJsonString(photo.sha256, out); out->push_back(',');
    AppendKey("width", out); out->append(std::to_string(photo.width)); out->push_back(',');
    AppendKey("height", out); out->append(std::to_string(photo.height)); out->push_back(',');
    AppendKey("format", out); AppendJsonString(photo.format, out);
    out->push_back('}');
}

ParseResult ParsePhoto(JsonReader* reader, PhotoEntry* photo) {
    if (!reader->Consume('{')) return Error(ParseStatus::kMalformed, *reader, "photo must be object");
    bool has_id = false, has_path = false, has_size = false, has_sha = false;
    bool has_width = false, has_height = false, has_format = false;
    while (true) {
        if (reader->Consume('}')) break;
        std::string key;
        if (!reader->ParseString(&key) || !reader->Consume(':')) {
            return Error(ParseStatus::kMalformed, *reader, "invalid photo field");
        }
        bool* seen = nullptr;
        if (key == "id") seen = &has_id;
        else if (key == "path") seen = &has_path;
        else if (key == "size") seen = &has_size;
        else if (key == "sha256") seen = &has_sha;
        else if (key == "width") seen = &has_width;
        else if (key == "height") seen = &has_height;
        else if (key == "format") seen = &has_format;
        else return Error(ParseStatus::kUnknownField, *reader, "unknown photo field");
        if (*seen) return Error(ParseStatus::kDuplicateField, *reader, "duplicate photo field");
        *seen = true;
        if (key == "id") {
            if (!reader->ParseString(&photo->id)) return Error(ParseStatus::kInvalidValue, *reader, "invalid photo id");
        } else if (key == "path") {
            if (!reader->ParseString(&photo->path)) return Error(ParseStatus::kInvalidValue, *reader, "invalid photo path");
        } else if (key == "size") {
            if (!reader->ParseUint32(&photo->size)) return Error(ParseStatus::kInvalidValue, *reader, "invalid photo size");
        } else if (key == "sha256") {
            if (!reader->ParseString(&photo->sha256)) return Error(ParseStatus::kInvalidValue, *reader, "invalid photo sha256");
        } else if (key == "width") {
            if (!reader->ParseUint32(&photo->width)) return Error(ParseStatus::kInvalidValue, *reader, "invalid photo width");
        } else if (key == "height") {
            if (!reader->ParseUint32(&photo->height)) return Error(ParseStatus::kInvalidValue, *reader, "invalid photo height");
        } else {
            if (!reader->ParseString(&photo->format)) return Error(ParseStatus::kInvalidValue, *reader, "invalid photo format");
        }
        reader->SkipWhitespace();
        if (reader->Consume('}')) break;
        if (!reader->Consume(',')) return Error(ParseStatus::kMalformed, *reader, "photo fields must be comma separated");
        if (reader->NextIs('}')) return Error(ParseStatus::kMalformed, *reader, "trailing photo comma");
    }
    if (!has_id || !has_path || !has_size || !has_sha || !has_width || !has_height || !has_format) {
        return Error(ParseStatus::kMissingField, *reader, "photo field missing");
    }
    if (!ValidateManifestString(photo->id, kMaxPhotoIdBytes) || !ValidatePath(photo->path) ||
        photo->size != kExpectedBmpSize || !IsValidManagedSha256(photo->sha256) ||
        !((photo->width == 800 && photo->height == 480) ||
          (photo->width == 480 && photo->height == 800)) ||
        photo->format != "bmp24-6color") {
        return Error(ParseStatus::kInvalidValue, *reader, "photo value outside protocol limits");
    }
    std::transform(photo->sha256.begin(), photo->sha256.end(), photo->sha256.begin(),
                   [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return Ok();
}

ParseResult ParseManifestInternal(const char* data, size_t length, Manifest* out,
                                  uint32_t default_display_interval_sec) {
    if (data == nullptr || out == nullptr) return Error(ParseStatus::kInvalidValue, 0, "null argument");
    if (length == 0) return Error(ParseStatus::kEmpty, 0, "empty manifest");
    if (length > kManifestMaxBytes) return Error(ParseStatus::kTooLarge, 0, "manifest exceeds 32 KiB");
    JsonReader reader(data, length);
    if (!reader.Consume('{')) return Error(ParseStatus::kMalformed, reader, "manifest must be object");
    bool has_schema = false, has_generation = false, has_revision = false, has_photos = false;
    bool has_interval = false;
    Manifest parsed;
    if (default_display_interval_sec < kMinIntervalSec || default_display_interval_sec > kMaxIntervalSec) {
        return Error(ParseStatus::kInvalidValue, reader, "default display interval outside protocol limits");
    }
    parsed.display_interval_sec = default_display_interval_sec;
    while (true) {
        if (reader.Consume('}')) break;
        std::string key;
        if (!reader.ParseString(&key) || !reader.Consume(':')) {
            return Error(ParseStatus::kMalformed, reader, "invalid manifest field");
        }
        if (key == "schema") {
            if (has_schema) return Error(ParseStatus::kDuplicateField, reader, "duplicate schema");
            has_schema = true;
            if (!reader.ParseUint32(&parsed.schema)) return Error(ParseStatus::kInvalidValue, reader, "schema must be integer");
        } else if (key == "generation") {
            if (has_generation) return Error(ParseStatus::kDuplicateField, reader, "duplicate generation");
            has_generation = true;
            if (!reader.ParseString(&parsed.generation)) return Error(ParseStatus::kInvalidValue, reader, "generation must be string");
        } else if (key == "revision") {
            if (has_revision) return Error(ParseStatus::kDuplicateField, reader, "duplicate revision");
            has_revision = true;
            if (!reader.ParseUint32(&parsed.revision)) return Error(ParseStatus::kInvalidValue, reader, "revision must be integer");
        } else if (key == "display_interval_sec") {
            if (has_interval) return Error(ParseStatus::kDuplicateField, reader, "duplicate display interval");
            has_interval = true;
            if (!reader.ParseUint32(&parsed.display_interval_sec)) return Error(ParseStatus::kInvalidValue, reader, "display interval must be integer");
        } else if (key == "photos") {
            if (has_photos) return Error(ParseStatus::kDuplicateField, reader, "duplicate photos");
            has_photos = true;
            if (!reader.Consume('[')) return Error(ParseStatus::kInvalidValue, reader, "photos must be array");
            while (true) {
                if (reader.Consume(']')) break;
                if (parsed.photos.size() >= kMaxPhotos) return Error(ParseStatus::kInvalidValue, reader, "too many photos");
                PhotoEntry photo;
                ParseResult result = ParsePhoto(&reader, &photo);
                if (!result.ok()) return result;
                for (size_t i = 0; i < parsed.photos.size(); ++i) {
                    if (parsed.photos[i].id == photo.id) return Error(ParseStatus::kInvalidValue, reader, "duplicate photo id");
                }
                parsed.photos.push_back(photo);
                if (reader.Consume(']')) break;
                if (!reader.Consume(',')) return Error(ParseStatus::kMalformed, reader, "photos must be comma separated");
                if (reader.NextIs(']')) return Error(ParseStatus::kMalformed, reader, "trailing photos comma");
            }
        } else {
            return Error(ParseStatus::kUnknownField, reader, "unknown manifest field");
        }
        reader.SkipWhitespace();
        if (reader.Consume('}')) break;
        if (!reader.Consume(',')) return Error(ParseStatus::kMalformed, reader, "manifest fields must be comma separated");
        if (reader.NextIs('}')) return Error(ParseStatus::kMalformed, reader, "trailing manifest comma");
    }
    if (!has_schema || !has_generation || !has_revision || !has_photos) {
        return Error(ParseStatus::kMissingField, reader, "manifest field missing");
    }
    if (parsed.schema != 1 || !ValidateManifestString(parsed.generation, kMaxGenerationBytes) ||
        (has_interval && (parsed.display_interval_sec < kMinIntervalSec || parsed.display_interval_sec > kMaxIntervalSec))) {
        return Error(ParseStatus::kInvalidValue, reader, "manifest value outside protocol limits");
    }
    parsed.has_display_interval = has_interval;
    parsed.normalized_json.reserve(length);
    parsed.normalized_json.push_back('{');
    AppendKey("schema", &parsed.normalized_json); parsed.normalized_json.append("1,");
    AppendKey("generation", &parsed.normalized_json); AppendJsonString(parsed.generation, &parsed.normalized_json);
    parsed.normalized_json.push_back(',');
    AppendKey("revision", &parsed.normalized_json); parsed.normalized_json.append(std::to_string(parsed.revision));
    if (has_interval) {
        parsed.normalized_json.push_back(',');
        AppendKey("display_interval_sec", &parsed.normalized_json);
        parsed.normalized_json.append(std::to_string(parsed.display_interval_sec));
    }
    parsed.normalized_json.append(",\"photos\":[");
    for (size_t i = 0; i < parsed.photos.size(); ++i) {
        if (i != 0) parsed.normalized_json.push_back(',');
        AppendPhoto(parsed.photos[i], &parsed.normalized_json);
    }
    parsed.normalized_json.append("]}");
    reader.SkipWhitespace();
    if (!reader.EndValue()) return Error(ParseStatus::kMalformed, reader, "trailing JSON data");
    *out = parsed;
    return Ok();
}

ParseResult ParseWifi(JsonReader* reader, PhotoPullConfig* config) {
    if (!reader->Consume('{')) return Error(ParseStatus::kInvalidValue, *reader, "wifi must be object");
    bool has_ssid = false, has_password = false;
    while (true) {
        if (reader->Consume('}')) break;
        std::string key;
        if (!reader->ParseString(&key) || !reader->Consume(':')) return Error(ParseStatus::kMalformed, *reader, "invalid wifi field");
        if (key == "ssid") {
            if (has_ssid) return Error(ParseStatus::kDuplicateField, *reader, "duplicate ssid");
            has_ssid = true;
            if (!reader->ParseString(&config->ssid) || config->ssid.size() > 32 || config->ssid.find('\0') != std::string::npos) return Error(ParseStatus::kInvalidValue, *reader, "ssid exceeds 32 bytes or contains NUL");
        } else if (key == "password") {
            if (has_password) return Error(ParseStatus::kDuplicateField, *reader, "duplicate password");
            has_password = true;
            if (!reader->ParseString(&config->password) || config->password.size() > 64 || config->password.find('\0') != std::string::npos) return Error(ParseStatus::kInvalidValue, *reader, "password exceeds 64 bytes or contains NUL");
        } else return Error(ParseStatus::kUnknownField, *reader, "unknown wifi field");
        if (reader->Consume('}')) break;
        if (!reader->Consume(',')) return Error(ParseStatus::kMalformed, *reader, "wifi fields must be comma separated");
        if (reader->NextIs('}')) return Error(ParseStatus::kMalformed, *reader, "trailing wifi comma");
    }
    if (!has_ssid) return Error(ParseStatus::kMissingField, *reader, "wifi ssid missing");
    config->has_wifi = !config->ssid.empty();
    return Ok();
}

ParseResult ParseServer(JsonReader* reader, PhotoPullConfig* config) {
    if (!reader->Consume('{')) return Error(ParseStatus::kInvalidValue, *reader, "server must be object");
    bool has_url = false, has_path = false, has_token = false, has_ca = false;
    while (true) {
        if (reader->Consume('}')) break;
        std::string key;
        if (!reader->ParseString(&key) || !reader->Consume(':')) return Error(ParseStatus::kMalformed, *reader, "invalid server field");
        if (key == "base_url") {
            if (has_url) return Error(ParseStatus::kDuplicateField, *reader, "duplicate base_url");
            has_url = true;
            std::string normalized_url;
            if (!reader->ParseString(&config->base_url) || !NormalizeHttpsOrigin(config->base_url, &normalized_url)) return Error(ParseStatus::kInvalidValue, *reader, "base_url must be a valid HTTPS origin");
            config->base_url = normalized_url;
        } else if (key == "manifest_path") {
            if (has_path) return Error(ParseStatus::kDuplicateField, *reader, "duplicate manifest_path");
            has_path = true;
            if (!reader->ParseString(&config->manifest_path) || !ValidatePath(config->manifest_path)) return Error(ParseStatus::kInvalidValue, *reader, "invalid manifest_path");
        } else if (key == "bearer_token") {
            if (has_token) return Error(ParseStatus::kDuplicateField, *reader, "duplicate bearer_token");
            has_token = true;
            if (!reader->ParseString(&config->bearer_token) || config->bearer_token.size() > 2048 || config->bearer_token.find('\0') != std::string::npos) return Error(ParseStatus::kInvalidValue, *reader, "bearer_token exceeds 2048 bytes or contains NUL");
            for (size_t i = 0; i < config->bearer_token.size(); ++i) if (static_cast<unsigned char>(config->bearer_token[i]) < 0x20 || static_cast<unsigned char>(config->bearer_token[i]) == 0x7f) return Error(ParseStatus::kInvalidValue, *reader, "bearer_token contains control character");
        } else if (key == "ca_file") {
            if (has_ca) return Error(ParseStatus::kDuplicateField, *reader, "duplicate ca_file");
            has_ca = true;
            if (!reader->ParseString(&config->ca_file) || config->ca_file.size() > 256 || !ValidatePath(config->ca_file) || config->ca_file.compare(0, 8, "/sdcard/") != 0) return Error(ParseStatus::kInvalidValue, *reader, "ca_file must be under /sdcard");
        } else return Error(ParseStatus::kUnknownField, *reader, "unknown server field");
        if (reader->Consume('}')) break;
        if (!reader->Consume(',')) return Error(ParseStatus::kMalformed, *reader, "server fields must be comma separated");
        if (reader->NextIs('}')) return Error(ParseStatus::kMalformed, *reader, "trailing server comma");
    }
    if (!has_url || !has_path) return Error(ParseStatus::kMissingField, *reader, "server field missing");
    config->has_server = true;
    return Ok();
}

ParseResult ParseConfigInternal(const char* data, size_t length, PhotoPullConfig* out) {
    if (data == nullptr || out == nullptr) return Error(ParseStatus::kInvalidValue, 0, "null argument");
    if (length == 0) return Error(ParseStatus::kEmpty, 0, "empty config");
    if (length > kConfigMaxBytes) return Error(ParseStatus::kTooLarge, 0, "config exceeds 8 KiB");
    JsonReader reader(data, length);
    if (!reader.Consume('{')) return Error(ParseStatus::kMalformed, reader, "config must be object");
    PhotoPullConfig parsed;
    bool has_schema = false, has_enabled = false, has_wifi = false, has_server = false;
    bool has_poll = false, has_display = false;
    while (true) {
        if (reader.Consume('}')) break;
        std::string key;
        if (!reader.ParseString(&key) || !reader.Consume(':')) return Error(ParseStatus::kMalformed, reader, "invalid config field");
        if (key == "schema") {
            if (has_schema) return Error(ParseStatus::kDuplicateField, reader, "duplicate schema");
            has_schema = true;
            if (!reader.ParseUint32(&parsed.schema)) return Error(ParseStatus::kInvalidValue, reader, "schema must be integer");
        } else if (key == "enabled") {
            if (has_enabled) return Error(ParseStatus::kDuplicateField, reader, "duplicate enabled");
            has_enabled = true;
            if (!reader.ParseBool(&parsed.enabled)) return Error(ParseStatus::kInvalidValue, reader, "enabled must be boolean");
        } else if (key == "wifi") {
            if (has_wifi) return Error(ParseStatus::kDuplicateField, reader, "duplicate wifi");
            has_wifi = true;
            ParseResult result = ParseWifi(&reader, &parsed);
            if (!result.ok()) return result;
        } else if (key == "server") {
            if (has_server) return Error(ParseStatus::kDuplicateField, reader, "duplicate server");
            has_server = true;
            ParseResult result = ParseServer(&reader, &parsed);
            if (!result.ok()) return result;
        } else if (key == "poll_interval_sec") {
            if (has_poll) return Error(ParseStatus::kDuplicateField, reader, "duplicate poll interval");
            has_poll = true;
            if (!reader.ParseUint32(&parsed.poll_interval_sec)) return Error(ParseStatus::kInvalidValue, reader, "poll interval must be integer");
        } else if (key == "default_display_interval_sec") {
            if (has_display) return Error(ParseStatus::kDuplicateField, reader, "duplicate display interval");
            has_display = true;
            if (!reader.ParseUint32(&parsed.default_display_interval_sec)) return Error(ParseStatus::kInvalidValue, reader, "display interval must be integer");
        } else return Error(ParseStatus::kUnknownField, reader, "unknown config field");
        if (reader.Consume('}')) break;
        if (!reader.Consume(',')) return Error(ParseStatus::kMalformed, reader, "config fields must be comma separated");
        if (reader.NextIs('}')) return Error(ParseStatus::kMalformed, reader, "trailing config comma");
    }
    if (!has_schema || !has_enabled || parsed.schema != 1 ||
        parsed.poll_interval_sec < kMinIntervalSec || parsed.poll_interval_sec > kMaxIntervalSec ||
        parsed.default_display_interval_sec < kMinIntervalSec || parsed.default_display_interval_sec > kMaxIntervalSec) {
        return Error(ParseStatus::kInvalidValue, reader, "config value outside protocol limits");
    }
    if (parsed.enabled && !has_server) return Error(ParseStatus::kMissingField, reader, "enabled config requires server");
    parsed.has_server = has_server;
    reader.SkipWhitespace();
    if (!reader.EndValue()) return Error(ParseStatus::kMalformed, reader, "trailing JSON data");
    *out = parsed;
    return Ok();
}

}  // namespace

ParseResult ParseManifestJson(const char* data, size_t length, Manifest* out,
                              uint32_t default_display_interval_sec) {
    return ParseManifestInternal(data, length, out, default_display_interval_sec);
}

ParseResult ParseConfigJson(const char* data, size_t length, PhotoPullConfig* out) {
    return ParseConfigInternal(data, length, out);
}

bool NormalizeSourceUrl(const std::string& source, std::string* normalized) {
    return NormalizeSourceUrlInternal(source, normalized);
}

ParseResult LoadConfig(ReadWholeFileFn read_file, void* context, const char* path,
                       PhotoPullConfig* out) {
    if (read_file == nullptr || path == nullptr || out == nullptr) return Error(ParseStatus::kInvalidValue, 0, "null argument");
    std::vector<uint8_t> data;
    if (!read_file(context, path, &data)) return Error(ParseStatus::kMalformed, 0, "cannot read config");
    return ParseConfigJson(reinterpret_cast<const char*>(data.data()), data.size(), out);
}

VersionDecision CompareManifest(const Manifest& candidate, const Manifest* current,
                                bool same_source) {
    if (current == nullptr || !same_source) return VersionDecision::kAccept;
    if (candidate.generation != current->generation) return VersionDecision::kAccept;
    if (candidate.revision < current->revision) return VersionDecision::kRejectRevisionRollback;
    if (candidate.revision > current->revision) return VersionDecision::kAccept;
    if (candidate.normalized_json == current->normalized_json) return VersionDecision::kNotModified;
    return VersionDecision::kRejectSameRevisionConflict;
}

bool IsValidManagedSha256(const std::string& sha256) {
    if (sha256.size() != 64) return false;
    for (size_t i = 0; i < sha256.size(); ++i) {
        if (!IsHex(sha256[i])) return false;
    }
    return true;
}

std::string ManagedImagePath(const std::string& sha256) {
    if (!IsValidManagedSha256(sha256)) return std::string();
    std::string lower = sha256;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(tolower(c));
    });
    return std::string(kManagedDirectory) + "/" + lower + ".bmp";
}

std::string ManagedPartPath(const std::string& sha256) {
    if (!IsValidManagedSha256(sha256)) return std::string();
    std::string lower = sha256;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(tolower(c));
    });
    return std::string(kManagedDirectory) + "/" + lower + ".part";
}

const char* ParseStatusString(ParseStatus status) {
    switch (status) {
        case ParseStatus::kOk: return "ok";
        case ParseStatus::kEmpty: return "empty";
        case ParseStatus::kTooLarge: return "too_large";
        case ParseStatus::kMalformed: return "malformed";
        case ParseStatus::kMissingField: return "missing_field";
        case ParseStatus::kDuplicateField: return "duplicate_field";
        case ParseStatus::kUnknownField: return "unknown_field";
        case ParseStatus::kInvalidValue: return "invalid_value";
    }
    return "unknown";
}

const char* VersionDecisionString(VersionDecision decision) {
    switch (decision) {
        case VersionDecision::kAccept: return "accept";
        case VersionDecision::kNotModified: return "not_modified";
        case VersionDecision::kRejectRevisionRollback: return "reject_revision_rollback";
        case VersionDecision::kRejectSameRevisionConflict: return "reject_same_revision_conflict";
    }
    return "unknown";
}

}  // namespace photopull
