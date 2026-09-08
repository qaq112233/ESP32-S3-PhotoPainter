#include "store.h"

#include <algorithm>
#include <string.h>

namespace photopull {

static const uint8_t kSnapshotMagic[8] = {'P', 'P', 'S', 'N', 'A', 'P', '1', 0};
static const uint32_t kSnapshotFormatVersion = 1;
static const size_t kSnapshotHeaderBytes = 64;
static const size_t kMaxSnapshotPayload = kManifestMaxBytes + kMaxSourceBytes +
                                           kMaxEtagBytes + 68;

// A small SHA-256 implementation keeps the core independent of ESP-IDF.  An
// adapter can replace it at the integration boundary if the platform already
// has an mbedTLS/OpenSSL streaming context; the on-disk format remains the
// standard SHA-256 digest.
struct HashState {
    uint32_t h[8];
    uint64_t bit_count;
    uint8_t block[64];
    size_t block_size;

    HashState() { Reset(); }

    void Reset() {
        h[0] = 0x6a09e667U; h[1] = 0xbb67ae85U; h[2] = 0x3c6ef372U; h[3] = 0xa54ff53aU;
        h[4] = 0x510e527fU; h[5] = 0x9b05688cU; h[6] = 0x1f83d9abU; h[7] = 0x5be0cd19U;
        bit_count = 0;
        block_size = 0;
    }

    static uint32_t Ror(uint32_t value, unsigned amount) {
        return (value >> amount) | (value << (32U - amount));
    }

    void Transform(const uint8_t* block_data) {
        static const uint32_t k[64] = {
            0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
            0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
            0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
            0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
            0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
            0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
            0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
            0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
            0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
            0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
            0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
            0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
            0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
            0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
            0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
            0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
        };
        uint32_t w[64];
        for (size_t i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(block_data[i * 4]) << 24) |
                   (static_cast<uint32_t>(block_data[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(block_data[i * 4 + 2]) << 8) |
                   static_cast<uint32_t>(block_data[i * 4 + 3]);
        }
        for (size_t i = 16; i < 64; ++i) {
            const uint32_t s0 = Ror(w[i - 15], 7) ^ Ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = Ror(w[i - 2], 17) ^ Ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (size_t i = 0; i < 64; ++i) {
            const uint32_t s1 = Ror(e, 6) ^ Ror(e, 11) ^ Ror(e, 25);
            const uint32_t ch = (e & f) ^ ((~e) & g);
            const uint32_t temp1 = hh + s1 + ch + k[i] + w[i];
            const uint32_t s0 = Ror(a, 2) ^ Ror(a, 13) ^ Ror(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = s0 + maj;
            hh = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    void Update(const uint8_t* data, size_t length) {
        if (data == nullptr && length != 0) return;
        bit_count += static_cast<uint64_t>(length) * 8U;
        while (length != 0) {
            const size_t take = std::min(length, sizeof(block) - block_size);
            memcpy(block + block_size, data, take);
            block_size += take;
            data += take;
            length -= take;
            if (block_size == sizeof(block)) {
                Transform(block);
                block_size = 0;
            }
        }
    }

    void Final(uint8_t digest[32]) {
        const uint64_t original_bits = bit_count;
        block[block_size++] = 0x80;
        if (block_size > 56) {
            while (block_size < 64) block[block_size++] = 0;
            Transform(block);
            block_size = 0;
        }
        while (block_size < 56) block[block_size++] = 0;
        for (int i = 7; i >= 0; --i) block[block_size++] = static_cast<uint8_t>(original_bits >> (i * 8));
        Transform(block);
        for (size_t i = 0; i < 8; ++i) {
            digest[i * 4] = static_cast<uint8_t>(h[i] >> 24);
            digest[i * 4 + 1] = static_cast<uint8_t>(h[i] >> 16);
            digest[i * 4 + 2] = static_cast<uint8_t>(h[i] >> 8);
            digest[i * 4 + 3] = static_cast<uint8_t>(h[i]);
        }
    }
};

namespace {

bool DigestsEqual(const uint8_t a[32], const uint8_t b[32]) {
    uint8_t difference = 0;
    for (size_t i = 0; i < 32; ++i) difference |= static_cast<uint8_t>(a[i] ^ b[i]);
    return difference == 0;
}

uint32_t Crc32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320U & -(crc & 1U));
    }
    return ~crc;
}

void AppendU32(std::vector<uint8_t>* out, uint32_t value) {
    out->push_back(static_cast<uint8_t>(value));
    out->push_back(static_cast<uint8_t>(value >> 8));
    out->push_back(static_cast<uint8_t>(value >> 16));
    out->push_back(static_cast<uint8_t>(value >> 24));
}

bool ReadU32(const std::vector<uint8_t>& bytes, size_t* offset, uint32_t* value) {
    if (*offset > bytes.size() || bytes.size() - *offset < 4) return false;
    *value = static_cast<uint32_t>(bytes[*offset]) |
             (static_cast<uint32_t>(bytes[*offset + 1]) << 8) |
             (static_cast<uint32_t>(bytes[*offset + 2]) << 16) |
             (static_cast<uint32_t>(bytes[*offset + 3]) << 24);
    *offset += 4;
    return true;
}

bool ReadU64(const std::vector<uint8_t>& bytes, size_t* offset, uint64_t* value) {
    if (*offset > bytes.size() || bytes.size() - *offset < 8) return false;
    *value = 0;
    for (int i = 0; i < 8; ++i) *value |= static_cast<uint64_t>(bytes[*offset + i]) << (i * 8);
    *offset += 8;
    return true;
}

void HashBytes(const uint8_t* data, size_t length, uint8_t digest[32]) {
    HashState state;
    state.Update(data, length);
    state.Final(digest);
}

bool IsPrintableMetadata(const std::string& value) {
    if (value.empty()) return false;
    for (size_t i = 0; i < value.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (c < 0x20 || c > 0x7e) return false;
    }
    return true;
}

}  // namespace

ImageWriter::ImageWriter()
    : store_(nullptr), bytes_written_(0), hash_(nullptr), finished_(false) {}

ImageWriter::ImageWriter(ImageWriter&& other)
    : store_(other.store_), expected_(other.expected_), part_path_(other.part_path_),
      bytes_written_(other.bytes_written_), hash_(other.hash_), finished_(other.finished_) {
    other.store_ = nullptr;
    other.hash_ = nullptr;
    other.finished_ = true;
}

ImageWriter& ImageWriter::operator=(ImageWriter&& other) {
    if (this == &other) return *this;
    Abort();
    store_ = other.store_;
    expected_ = other.expected_;
    part_path_ = other.part_path_;
    bytes_written_ = other.bytes_written_;
    hash_ = other.hash_;
    finished_ = other.finished_;
    other.store_ = nullptr;
    other.hash_ = nullptr;
    other.finished_ = true;
    return *this;
}

ImageWriter::~ImageWriter() {
    Abort();
}

StoreStatus ImageWriter::Write(const uint8_t* data, size_t length) {
    if (store_ == nullptr || finished_ || (data == nullptr && length != 0)) return StoreStatus::kInvalidArgument;
    if (bytes_written_ > expected_.size || length > expected_.size - bytes_written_) return StoreStatus::kIntegrityError;
    size_t written = 0;
    const StoreStatus status = store_->WriteImagePart(this, data, length, true, &written);
    if (status != StoreStatus::kOk || written != length) return status == StoreStatus::kOk ? StoreStatus::kIoError : status;
    if (bytes_written_ > UINT64_MAX - length) return StoreStatus::kInvalidArgument;
    bytes_written_ += length;
    hash_->Update(data, length);
    return StoreStatus::kOk;
}

StoreStatus ImageWriter::Finish() {
    if (store_ == nullptr || finished_) return StoreStatus::kInvalidArgument;
    const StoreStatus result = store_->FinishImage(this);
    if (result == StoreStatus::kOk) {
        finished_ = true;
        store_ = nullptr;
        delete hash_;
        hash_ = nullptr;
    }
    return result;
}

void ImageWriter::Abort() {
    if (hash_ != nullptr) {
        delete hash_;
        hash_ = nullptr;
    }
    if (store_ != nullptr && !part_path_.empty()) {
        // Best-effort cleanup.  A failed remove is harmless: a .part file is
        // never considered a published image and GC handles it after mirroring.
        store_->fs_->Remove(part_path_);
    }
    store_ = nullptr;
    finished_ = true;
}

Store::Store(FileSystem* fs, PhotoValidator validator, void* validator_context,
             uint32_t default_display_interval_sec)
    : fs_(fs), validator_(validator), validator_context_(validator_context),
      default_display_interval_sec_(default_display_interval_sec) {
    if (default_display_interval_sec_ < kMinIntervalSec ||
        default_display_interval_sec_ > kMaxIntervalSec) {
        default_display_interval_sec_ = kDefaultDisplayIntervalSec;
    }
}

Store::~Store() {}

std::string Store::SlotPath(int slot) const {
    return std::string(kManagedDirectory) + (slot == 0 ? "/snapshot_a.bin" : "/snapshot_b.bin");
}

std::string Store::SlotTempPath(int slot) const {
    return std::string(kManagedDirectory) + (slot == 0 ? "/snapshot_a.tmp" : "/snapshot_b.tmp");
}

StoreStatus Store::EnsureDirectory() {
    return fs_ == nullptr ? StoreStatus::kInvalidArgument : StoreStatus::kOk;
}

StoreStatus Store::WriteImagePart(const ImageWriter* writer, const uint8_t* data,
                                  size_t length, bool append, size_t* bytes_written) {
    if (writer == nullptr || fs_ == nullptr || data == nullptr || bytes_written == nullptr) return StoreStatus::kInvalidArgument;
    *bytes_written = 0;
    if (!fs_->WriteFile(writer->part_path_, data, length, append, bytes_written)) return StoreStatus::kIoError;
    return *bytes_written == length ? StoreStatus::kOk : StoreStatus::kIoError;
}

StoreStatus Store::BeginImage(const PhotoEntry& expected, ImageWriter* out) {
    if (out == nullptr || fs_ == nullptr || !IsValidManagedSha256(expected.sha256) ||
        expected.size != kExpectedBmpSize) return StoreStatus::kInvalidArgument;
    if (out->store_ != nullptr) return StoreStatus::kBusy;
    const std::string final_path = ManagedImagePath(expected.sha256);
    const std::string part_path = ManagedPartPath(expected.sha256);
    size_t existing_size = 0;
    if (fs_->StatFile(final_path, &existing_size)) {
        bool valid = false;
        if (CheckPhoto(expected, &valid) == StoreStatus::kOk && valid) return StoreStatus::kAlreadyExists;
        if (!fs_->Remove(final_path)) return StoreStatus::kIoError;
    }
    // Remove a stale part from a prior interrupted transfer.  A part is never
    // part of the committed library, so this is safe and bounded.
    if (fs_->StatFile(part_path, &existing_size) && !fs_->Remove(part_path)) return StoreStatus::kIoError;
    *out = ImageWriter();
    out->store_ = this;
    out->expected_ = expected;
    out->part_path_ = part_path;
    out->bytes_written_ = 0;
    out->hash_ = new HashState();
    out->finished_ = false;
    return StoreStatus::kOk;
}

StoreStatus Store::FinishImage(ImageWriter* writer) {
    if (writer == nullptr || writer->store_ != this || writer->hash_ == nullptr) return StoreStatus::kInvalidArgument;
    if (writer->bytes_written_ != writer->expected_.size) return StoreStatus::kIntegrityError;
    uint8_t digest[32];
    writer->hash_->Final(digest);
    char digest_hex[65];
    static const char kHex[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; ++i) {
        digest_hex[i * 2] = kHex[digest[i] >> 4];
        digest_hex[i * 2 + 1] = kHex[digest[i] & 0x0F];
    }
    digest_hex[64] = 0;
    if (writer->expected_.sha256.compare(digest_hex) != 0) return StoreStatus::kIntegrityError;
    if (!fs_->SyncFile(writer->part_path_)) return StoreStatus::kIoError;
    if (validator_ != nullptr && !validator_(fs_, writer->part_path_, writer->expected_, validator_context_)) {
        return StoreStatus::kImageInvalid;
    }
    const std::string final_path = ManagedImagePath(writer->expected_.sha256);
    if (!fs_->Rename(writer->part_path_, final_path)) return StoreStatus::kIoError;
    if (!fs_->SyncDirectory(kManagedDirectory)) return StoreStatus::kIoError;
    size_t final_size = 0;
    if (!fs_->StatFile(final_path, &final_size) || final_size != writer->expected_.size) return StoreStatus::kIoError;
    return StoreStatus::kOk;
}

namespace {
struct PhotoHashContext {
    HashState state;
    uint64_t size = 0;
};

bool HashChunk(const uint8_t* data, size_t length, void* context) {
    PhotoHashContext* hash = static_cast<PhotoHashContext*>(context);
    if (hash == nullptr || (data == nullptr && length != 0) || hash->size > UINT64_MAX - length) return false;
    hash->state.Update(data, length);
    hash->size += length;
    return true;
}
}  // namespace

StoreStatus Store::ValidatePhotoInternal(const PhotoEntry& expected, bool* valid) {
    if (valid == nullptr || fs_ == nullptr || !IsValidManagedSha256(expected.sha256)) return StoreStatus::kInvalidArgument;
    *valid = false;
    const std::string path = ManagedImagePath(expected.sha256);
    size_t size = 0;
    if (!fs_->StatFile(path, &size)) return StoreStatus::kNotFound;
    if (size != expected.size || size != kExpectedBmpSize) return StoreStatus::kIntegrityError;
    PhotoHashContext hash;
    if (!fs_->ReadFileChunks(path, HashChunk, &hash)) return StoreStatus::kIoError;
    uint8_t digest[32];
    hash.state.Final(digest);
    char digest_hex[65];
    static const char kHex[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; ++i) {
        digest_hex[i * 2] = kHex[digest[i] >> 4];
        digest_hex[i * 2 + 1] = kHex[digest[i] & 0x0F];
    }
    digest_hex[64] = 0;
    if (hash.size != expected.size || expected.sha256 != digest_hex) return StoreStatus::kIntegrityError;
    if (validator_ != nullptr && !validator_(fs_, path, expected, validator_context_)) return StoreStatus::kImageInvalid;
    *valid = true;
    return StoreStatus::kOk;
}

StoreStatus Store::CheckPhoto(const PhotoEntry& expected, bool* valid) {
    return ValidatePhotoInternal(expected, valid);
}

StoreStatus Store::ValidateSnapshotImages(const SnapshotInfo& snapshot,
                                          std::vector<bool>* valid, bool* complete) {
    if (valid == nullptr || complete == nullptr) return StoreStatus::kInvalidArgument;
    valid->clear();
    valid->reserve(snapshot.manifest.photos.size());
    *complete = true;
    for (size_t i = 0; i < snapshot.manifest.photos.size(); ++i) {
        bool photo_valid = false;
        const StoreStatus status = CheckPhoto(snapshot.manifest.photos[i], &photo_valid);
        // A missing/corrupt photo is a recoverable incomplete snapshot.  A
        // filesystem error still makes the snapshot incomplete but does not
        // invalidate its manifest as a repair reference.
        (void)status;
        valid->push_back(photo_valid);
        if (!photo_valid) *complete = false;
    }
    return StoreStatus::kOk;
}

StoreStatus Store::ReadSlot(int slot, SlotRecord* out) {
    if (out == nullptr || (slot != 0 && slot != 1) || fs_ == nullptr) return StoreStatus::kInvalidArgument;
    *out = SlotRecord();
    std::vector<uint8_t> bytes;
    if (!fs_->ReadFile(SlotPath(slot), &bytes)) return StoreStatus::kNoSnapshot;
    SnapshotInfo snapshot;
    if (ParseSnapshotBytes(bytes, &snapshot) != StoreStatus::kOk) return StoreStatus::kIntegrityError;
    out->structural_valid = true;
    out->snapshot = snapshot;
    ValidateSnapshotImages(snapshot, &out->valid_images, &out->complete);
    return StoreStatus::kOk;
}

StoreStatus Store::ValidateSnapshot(const std::string& path, SnapshotInfo* out) {
    if (fs_ == nullptr || out == nullptr) return StoreStatus::kInvalidArgument;
    std::vector<uint8_t> bytes;
    if (!fs_->ReadFile(path, &bytes)) return StoreStatus::kNoSnapshot;
    return ParseSnapshotBytes(bytes, out);
}

StoreStatus Store::Recover(RecoveryState* out) {
    if (fs_ == nullptr || out == nullptr) return StoreStatus::kInvalidArgument;
    SlotRecord slots[2];
    ReadSlot(0, &slots[0]);
    ReadSlot(1, &slots[1]);
    int newest_structural = -1;
    int newest_complete = -1;
    for (int i = 0; i < 2; ++i) {
        if (!slots[i].structural_valid) continue;
        if (newest_structural < 0 ||
            slots[i].snapshot.commit_seq > slots[newest_structural].snapshot.commit_seq) {
            newest_structural = i;
        }
        if (slots[i].complete &&
            (newest_complete < 0 ||
             slots[i].snapshot.commit_seq > slots[newest_complete].snapshot.commit_seq)) {
            newest_complete = i;
        }
    }
    RecoveryState state;
    // A complete snapshot is the only safe offline library.  Prefer the
    // newest complete slot even when a newer structurally valid slot has a
    // missing/corrupt image; the latter remains an explicit repair basis
    // below instead of being silently discarded.
    const int selected = newest_complete >= 0 ? newest_complete : newest_structural;
    if (selected < 0) {
        recovery_ = state;
        *out = state;
        return StoreStatus::kNoSnapshot;
    }
    state.has_snapshot = true;
    state.complete = slots[selected].complete;
    state.active_slot = selected;
    state.snapshot = slots[selected].snapshot;
    state.valid_images = slots[selected].valid_images;
    if (newest_complete >= 0 && newest_structural >= 0 &&
        newest_structural != newest_complete &&
        slots[newest_structural].snapshot.commit_seq >
            slots[newest_complete].snapshot.commit_seq &&
        !slots[newest_structural].complete) {
        state.has_newer_incomplete = true;
        state.newer_incomplete_slot = newest_structural;
        state.newer_incomplete_snapshot = slots[newest_structural].snapshot;
        state.newer_incomplete_valid_images = slots[newest_structural].valid_images;
    }
    const bool slots_mirrored = slots[0].structural_valid && slots[1].structural_valid &&
                                SameSnapshot(slots[0].snapshot, slots[1].snapshot) &&
                                slots[0].complete && slots[1].complete;
    // A newer incomplete record needs content repair first.  Mirroring the
    // older complete record over it would erase the revision/manifest basis
    // needed to reject rollback and to retry the same revision.  Therefore it
    // is GC-pending, but not mirror-pending, until a candidate repairs it.
    state.mirror_pending = !slots_mirrored && !state.has_newer_incomplete;
    state.gc_pending = !slots_mirrored;
    if (slots_mirrored) {
        // A reset can occur after both slots were mirrored but during cleanup.
        // Keep this separate from mirror_pending: downloaded candidate files
        // are intentionally retained and must not block the next commit.
        std::vector<std::string> paths;
        if (!fs_->ListFiles(kManagedDirectory, &paths)) {
            state.gc_pending = true;
        } else {
            for (size_t i = 0; i < paths.size() && !state.gc_pending; ++i) {
                if (paths[i] == SlotTempPath(0) || paths[i] == SlotTempPath(1)) {
                    state.gc_pending = true;
                    continue;
                }
                std::string suffix, digest;
                if (!IsManagedImageName(paths[i], &suffix, &digest)) continue;
                if (suffix == ".part") {
                    state.gc_pending = true;
                    break;
                }
                bool referenced = false;
                for (size_t j = 0; j < state.snapshot.manifest.photos.size(); ++j) {
                    if (state.snapshot.manifest.photos[j].sha256 == digest) {
                        referenced = true;
                        break;
                    }
                }
                if (!referenced) state.gc_pending = true;
            }
        }
    }
    recovery_ = state;
    *out = state;
    return state.complete ? StoreStatus::kOk : StoreStatus::kNoCompleteSnapshot;
}

StoreStatus Store::PlanCandidate(const Manifest& candidate, const std::string& source,
                                 CandidatePlan* out) {
    std::string normalized_source;
    if (out == nullptr || fs_ == nullptr ||
        !NormalizeSourceUrl(source, &normalized_source)) return StoreStatus::kInvalidArgument;
    *out = CandidatePlan();
    const SnapshotInfo* current_snapshot = nullptr;
    if (recovery_.has_newer_incomplete) {
        current_snapshot = &recovery_.newer_incomplete_snapshot;
    } else if (recovery_.has_snapshot) {
        current_snapshot = &recovery_.snapshot;
    }
    const Manifest* current = current_snapshot == nullptr ? nullptr : &current_snapshot->manifest;
    const bool same_source = current_snapshot != nullptr && normalized_source == current_snapshot->source;
    out->decision = CompareManifest(candidate, current, same_source);
    if (out->decision == VersionDecision::kRejectRevisionRollback) return StoreStatus::kVersionRejected;
    if (out->decision == VersionDecision::kRejectSameRevisionConflict) return StoreStatus::kSameRevisionConflict;
    std::vector<std::string> missing_digests;
    for (size_t i = 0; i < candidate.photos.size(); ++i) {
        MissingPhoto item;
        item.photo = candidate.photos[i];
        bool valid = false;
        const StoreStatus status = CheckPhoto(item.photo, &valid);
        item.valid = valid;
        item.present = status != StoreStatus::kNotFound;
        if (!valid) {
            // Content addressing means several IDs can share one download.
            // Count each SHA once for space planning; keep one MissingPhoto per
            // ID so the caller still preserves manifest order.
            if (std::find(missing_digests.begin(), missing_digests.end(), item.photo.sha256) == missing_digests.end()) {
                if (out->total_missing_bytes > SIZE_MAX - item.photo.size) return StoreStatus::kInvalidArgument;
                out->total_missing_bytes += item.photo.size;
                missing_digests.push_back(item.photo.sha256);
            }
        }
        out->photos.push_back(item);
    }
    out->needs_repair = recovery_.has_snapshot &&
                        (!recovery_.complete || recovery_.has_newer_incomplete);
    if (out->decision == VersionDecision::kNotModified && out->total_missing_bytes == 0 && !out->needs_repair) {
        return StoreStatus::kOk;
    }
    // A same-revision candidate with missing content must be downloaded again;
    // this is also how the HTTPS layer knows a 304 cannot be trusted locally.
    if (out->decision == VersionDecision::kNotModified && out->total_missing_bytes != 0) out->needs_repair = true;
    return StoreStatus::kOk;
}

StoreStatus Store::BuildSnapshot(const Manifest& manifest, const std::string& source,
                                 const std::string& etag, uint64_t commit_seq,
                                 std::vector<uint8_t>* bytes) {
    std::string normalized_source;
    if (bytes == nullptr || !NormalizeSourceUrl(source, &normalized_source) ||
        etag.size() > kMaxEtagBytes || (!etag.empty() && !IsPrintableMetadata(etag)) ||
        manifest.normalized_json.empty() || manifest.normalized_json.size() > kManifestMaxBytes ||
        commit_seq == 0) return StoreStatus::kInvalidArgument;
    std::vector<uint8_t> payload;
    payload.reserve(16 + normalized_source.size() + etag.size() + manifest.normalized_json.size());
    const uint32_t effective_interval = manifest.display_interval_sec;
    if (effective_interval < kMinIntervalSec || effective_interval > kMaxIntervalSec) return StoreStatus::kInvalidArgument;
    AppendU32(&payload, effective_interval);
    AppendU32(&payload, static_cast<uint32_t>(normalized_source.size()));
    AppendU32(&payload, static_cast<uint32_t>(etag.size()));
    AppendU32(&payload, static_cast<uint32_t>(manifest.normalized_json.size()));
    payload.insert(payload.end(), normalized_source.begin(), normalized_source.end());
    payload.insert(payload.end(), etag.begin(), etag.end());
    payload.insert(payload.end(), manifest.normalized_json.begin(), manifest.normalized_json.end());
    if (payload.size() > kMaxSnapshotPayload) return StoreStatus::kInvalidArgument;
    uint8_t payload_digest[32];
    HashBytes(payload.data(), payload.size(), payload_digest);
    bytes->assign(kSnapshotHeaderBytes, 0);
    memcpy(bytes->data(), kSnapshotMagic, sizeof(kSnapshotMagic));
    (*bytes)[8] = static_cast<uint8_t>(kSnapshotFormatVersion);
    (*bytes)[12] = static_cast<uint8_t>(commit_seq);
    (*bytes)[13] = static_cast<uint8_t>(commit_seq >> 8);
    (*bytes)[14] = static_cast<uint8_t>(commit_seq >> 16);
    (*bytes)[15] = static_cast<uint8_t>(commit_seq >> 24);
    (*bytes)[16] = static_cast<uint8_t>(commit_seq >> 32);
    (*bytes)[17] = static_cast<uint8_t>(commit_seq >> 40);
    (*bytes)[18] = static_cast<uint8_t>(commit_seq >> 48);
    (*bytes)[19] = static_cast<uint8_t>(commit_seq >> 56);
    const uint32_t payload_length = static_cast<uint32_t>(payload.size());
    (*bytes)[20] = static_cast<uint8_t>(payload_length);
    (*bytes)[21] = static_cast<uint8_t>(payload_length >> 8);
    (*bytes)[22] = static_cast<uint8_t>(payload_length >> 16);
    (*bytes)[23] = static_cast<uint8_t>(payload_length >> 24);
    memcpy(bytes->data() + 24, payload_digest, sizeof(payload_digest));
    const uint32_t header_crc = Crc32(bytes->data(), 56);
    (*bytes)[56] = static_cast<uint8_t>(header_crc);
    (*bytes)[57] = static_cast<uint8_t>(header_crc >> 8);
    (*bytes)[58] = static_cast<uint8_t>(header_crc >> 16);
    (*bytes)[59] = static_cast<uint8_t>(header_crc >> 24);
    bytes->insert(bytes->end(), payload.begin(), payload.end());
    return StoreStatus::kOk;
}

StoreStatus Store::ParseSnapshotBytes(const std::vector<uint8_t>& bytes,
                                      SnapshotInfo* out) {
    if (out == nullptr || bytes.size() < kSnapshotHeaderBytes || bytes.size() > kSnapshotHeaderBytes + kMaxSnapshotPayload) return StoreStatus::kIntegrityError;
    if (memcmp(bytes.data(), kSnapshotMagic, sizeof(kSnapshotMagic)) != 0 || bytes[8] != kSnapshotFormatVersion) return StoreStatus::kIntegrityError;
    uint64_t commit_seq = 0;
    size_t offset = 12;
    if (!ReadU64(bytes, &offset, &commit_seq) || commit_seq == 0) return StoreStatus::kIntegrityError;
    uint32_t payload_length = 0;
    if (!ReadU32(bytes, &offset, &payload_length) || payload_length > kMaxSnapshotPayload ||
        bytes.size() != kSnapshotHeaderBytes + payload_length) return StoreStatus::kIntegrityError;
    const uint32_t stored_crc = static_cast<uint32_t>(bytes[56]) |
                                (static_cast<uint32_t>(bytes[57]) << 8) |
                                (static_cast<uint32_t>(bytes[58]) << 16) |
                                (static_cast<uint32_t>(bytes[59]) << 24);
    std::vector<uint8_t> header(bytes.begin(), bytes.begin() + 56);
    if (Crc32(header.data(), header.size()) != stored_crc) return StoreStatus::kIntegrityError;
    uint8_t payload_digest[32];
    memcpy(payload_digest, bytes.data() + 24, sizeof(payload_digest));
    uint8_t calculated_payload_digest[32];
    HashBytes(bytes.data() + kSnapshotHeaderBytes, payload_length, calculated_payload_digest);
    if (!DigestsEqual(payload_digest, calculated_payload_digest)) return StoreStatus::kIntegrityError;
    std::vector<uint8_t> payload(bytes.begin() + kSnapshotHeaderBytes, bytes.end());
    size_t payload_offset = 0;
    uint32_t effective_interval = 0, source_length = 0, etag_length = 0, manifest_length = 0;
    if (!ReadU32(payload, &payload_offset, &effective_interval) ||
        !ReadU32(payload, &payload_offset, &source_length) || !ReadU32(payload, &payload_offset, &etag_length) ||
        !ReadU32(payload, &payload_offset, &manifest_length) || source_length == 0 || source_length > kMaxSourceBytes ||
        effective_interval < kMinIntervalSec || effective_interval > kMaxIntervalSec ||
        etag_length > kMaxEtagBytes || manifest_length == 0 || manifest_length > kManifestMaxBytes ||
        payload_offset > payload.size() || payload.size() - payload_offset != static_cast<size_t>(source_length) + etag_length + manifest_length) {
        return StoreStatus::kIntegrityError;
    }
    SnapshotInfo parsed;
    parsed.commit_seq = commit_seq;
    parsed.source.assign(reinterpret_cast<const char*>(payload.data() + payload_offset), source_length);
    payload_offset += source_length;
    parsed.etag.assign(reinterpret_cast<const char*>(payload.data() + payload_offset), etag_length);
    payload_offset += etag_length;
    const char* manifest_data = reinterpret_cast<const char*>(payload.data() + payload_offset);
    ParseResult result = ParseManifestJson(manifest_data, manifest_length, &parsed.manifest,
                                            effective_interval);
    std::string normalized_source;
    if (!result.ok() || parsed.manifest.normalized_json.size() != manifest_length ||
        memcmp(parsed.manifest.normalized_json.data(), manifest_data, manifest_length) != 0 ||
        !NormalizeSourceUrl(parsed.source, &normalized_source) || normalized_source != parsed.source ||
        (!parsed.etag.empty() && !IsPrintableMetadata(parsed.etag))) return StoreStatus::kIntegrityError;
    memcpy(parsed.payload_sha256, payload_digest, sizeof(payload_digest));
    *out = parsed;
    return StoreStatus::kOk;
}

StoreStatus Store::WriteSnapshotToSlot(const std::vector<uint8_t>& bytes, int slot,
                                       SnapshotInfo* verified) {
    if (fs_ == nullptr || (slot != 0 && slot != 1) || verified == nullptr) return StoreStatus::kInvalidArgument;
    const std::string temp = SlotTempPath(slot);
    const std::string path = SlotPath(slot);
    size_t written = 0;
    if (!fs_->WriteFile(temp, bytes.data(), bytes.size(), false, &written) || written != bytes.size()) {
        fs_->Remove(temp);
        return StoreStatus::kIoError;
    }
    if (!fs_->SyncFile(temp)) {
        fs_->Remove(temp);
        return StoreStatus::kIoError;
    }
    std::vector<uint8_t> check;
    if (!fs_->ReadFile(temp, &check) || ParseSnapshotBytes(check, verified) != StoreStatus::kOk) {
        fs_->Remove(temp);
        return StoreStatus::kIntegrityError;
    }
    // FatFS does not guarantee POSIX replace-on-rename.  This is the slot
    // selected by the caller (the other slot remains the recovery copy), so
    // unlinking the old target cannot remove the only committed snapshot.
    size_t old_size = 0;
    if (fs_->StatFile(path, &old_size) && !fs_->Remove(path)) return StoreStatus::kIoError;
    if (!fs_->Rename(temp, path) || !fs_->SyncDirectory(kManagedDirectory)) return StoreStatus::kIoError;
    std::vector<uint8_t> after;
    if (!fs_->ReadFile(path, &after) || ParseSnapshotBytes(after, verified) != StoreStatus::kOk) return StoreStatus::kIntegrityError;
    return StoreStatus::kOk;
}

StoreStatus Store::MirrorSnapshot(const std::vector<uint8_t>& bytes, int slot,
                                  SnapshotInfo* verified) {
    return WriteSnapshotToSlot(bytes, slot, verified);
}

CommitResult Store::Commit(const Manifest& manifest, const std::string& source,
                           const std::string& etag, bool defer_mirror) {
    CommitResult result;
    std::string normalized_source;
    if (fs_ == nullptr || !NormalizeSourceUrl(source, &normalized_source) || etag.size() > kMaxEtagBytes) {
        result.status = StoreStatus::kInvalidArgument;
        return result;
    }
    CandidatePlan plan;
    const StoreStatus plan_status = PlanCandidate(manifest, normalized_source, &plan);
    if (plan_status == StoreStatus::kVersionRejected || plan_status == StoreStatus::kSameRevisionConflict) {
        result.status = plan_status;
        return result;
    }
    if (plan_status != StoreStatus::kOk) {
        result.status = plan_status;
        return result;
    }
    if (plan.decision == VersionDecision::kNotModified && plan.total_missing_bytes == 0 && !plan.needs_repair) {
        // The caller received an identical complete library.  Treat this as
        // an accepted no-op so a 200 response behaves like a 304 response.
        result.status = StoreStatus::kOk;
        result.committed = true;
        result.mirrored = !recovery_.mirror_pending;
        result.commit_seq = recovery_.snapshot.commit_seq;
        return result;
    }
    // A complete logical commit whose second slot or cleanup is pending is
    // repaired before accepting another server version.  This preserves the
    // invariant that an update is never followed by GC with only one durable
    // snapshot copy.
    if (recovery_.has_snapshot && recovery_.mirror_pending && recovery_.complete) {
        result.status = StoreStatus::kMirrorPending;
        return result;
    }
    for (size_t i = 0; i < manifest.photos.size(); ++i) {
        bool valid = false;
        if (CheckPhoto(manifest.photos[i], &valid) != StoreStatus::kOk || !valid) {
            result.status = StoreStatus::kImageInvalid;
            return result;
        }
    }
    SlotRecord slots[2];
    ReadSlot(0, &slots[0]);
    ReadSlot(1, &slots[1]);
    uint64_t highest_seq = 0;
    for (int i = 0; i < 2; ++i) if (slots[i].structural_valid) highest_seq = std::max(highest_seq, slots[i].snapshot.commit_seq);
    if (highest_seq == UINT64_MAX) {
        result.status = StoreStatus::kInvalidArgument;
        return result;
    }
    const uint64_t next_seq = highest_seq + 1;
    int target = recovery_.active_slot == 0 ? 1 : 0;
    if (recovery_.active_slot < 0) {
        if (!slots[0].structural_valid) target = 0;
        else if (!slots[1].structural_valid) target = 1;
        else target = slots[0].snapshot.commit_seq <= slots[1].snapshot.commit_seq ? 0 : 1;
    }
    std::vector<uint8_t> bytes;
    StoreStatus status = BuildSnapshot(manifest, normalized_source, etag, next_seq, &bytes);
    if (status != StoreStatus::kOk) {
        result.status = status;
        return result;
    }
    SnapshotInfo committed_snapshot;
    status = WriteSnapshotToSlot(bytes, target, &committed_snapshot);
    if (status != StoreStatus::kOk) {
        result.status = status;
        return result;
    }
    recovery_.has_snapshot = true;
    recovery_.complete = true;
    recovery_.active_slot = target;
    recovery_.snapshot = committed_snapshot;
    recovery_.valid_images.assign(manifest.photos.size(), true);
    recovery_.has_newer_incomplete = false;
    recovery_.newer_incomplete_slot = -1;
    recovery_.newer_incomplete_snapshot = SnapshotInfo();
    recovery_.newer_incomplete_valid_images.clear();
    recovery_.mirror_pending = true;
    recovery_.gc_pending = false;
    result.commit_seq = next_seq;
    result.committed = true;
    if (defer_mirror) {
        result.status = StoreStatus::kMirrorPending;
        result.mirrored = false;
        result.gc_pending = true;
        return result;
    }
    const int mirror_slot = 1 - target;
    SnapshotInfo mirrored_snapshot;
    status = MirrorSnapshot(bytes, mirror_slot, &mirrored_snapshot);
    if (status != StoreStatus::kOk || !SameSnapshot(committed_snapshot, mirrored_snapshot)) {
        result.status = StoreStatus::kMirrorPending;
        result.mirrored = false;
        result.gc_pending = true;
        return result;
    }
    recovery_.mirror_pending = false;
    result.mirrored = true;
    status = GarbageCollect();
    result.gc_pending = status != StoreStatus::kOk;
    recovery_.mirror_pending = false;
    recovery_.gc_pending = result.gc_pending;
    result.status = StoreStatus::kOk;
    return result;
}

bool Store::SameSnapshot(const SnapshotInfo& a, const SnapshotInfo& b) {
    return a.commit_seq == b.commit_seq && a.source == b.source && a.etag == b.etag &&
           a.manifest.normalized_json == b.manifest.normalized_json &&
           DigestsEqual(a.payload_sha256, b.payload_sha256);
}

StoreStatus Store::RepairMirror(bool* mirrored, bool* gc_pending) {
    if (mirrored == nullptr || gc_pending == nullptr || fs_ == nullptr) return StoreStatus::kInvalidArgument;
    *mirrored = false;
    *gc_pending = false;
    if (recovery_.has_newer_incomplete) {
        // Keep the newer damaged slot intact.  Replacing it with the older
        // playback snapshot would lose the revision and manifest needed for
        // a same-version repair or rollback rejection.
        *gc_pending = true;
        return StoreStatus::kMirrorPending;
    }
    if (!recovery_.has_snapshot || !recovery_.complete || recovery_.active_slot < 0) return StoreStatus::kNoCompleteSnapshot;
    std::vector<uint8_t> bytes;
    if (!fs_->ReadFile(SlotPath(recovery_.active_slot), &bytes)) return StoreStatus::kIoError;
    SnapshotInfo source_snapshot;
    if (ParseSnapshotBytes(bytes, &source_snapshot) != StoreStatus::kOk || !SameSnapshot(source_snapshot, recovery_.snapshot)) return StoreStatus::kIntegrityError;
    std::vector<bool> current_valid;
    bool current_complete = false;
    ValidateSnapshotImages(source_snapshot, &current_valid, &current_complete);
    if (!current_complete) {
        recovery_.complete = false;
        recovery_.valid_images = current_valid;
        return StoreStatus::kNoCompleteSnapshot;
    }
    SnapshotInfo mirrored_snapshot;
    const StoreStatus status = MirrorSnapshot(bytes, 1 - recovery_.active_slot, &mirrored_snapshot);
    if (status != StoreStatus::kOk || !SameSnapshot(source_snapshot, mirrored_snapshot)) return StoreStatus::kMirrorPending;
    recovery_.mirror_pending = false;
    recovery_.gc_pending = false;
    *mirrored = true;
    const StoreStatus gc_status = GarbageCollect();
    *gc_pending = gc_status != StoreStatus::kOk;
    recovery_.mirror_pending = false;
    recovery_.gc_pending = *gc_pending;
    return gc_status == StoreStatus::kOk ? StoreStatus::kOk : StoreStatus::kIoError;
}

bool Store::IsManagedImageName(const std::string& path, std::string* suffix,
                               std::string* digest) {
    const std::string prefix = std::string(kManagedDirectory) + "/";
    if (path.compare(0, prefix.size(), prefix) != 0) return false;
    const std::string name = path.substr(prefix.size());
    if (name.empty() || name.find('/') != std::string::npos) return false;
    const size_t dot = name.find_last_of('.');
    if (dot != 64 || (name.size() != 68 && name.size() != 69)) return false;
    const std::string stem = name.substr(0, dot);
    for (size_t i = 0; i < stem.size(); ++i) {
        if (!((stem[i] >= '0' && stem[i] <= '9') ||
              (stem[i] >= 'a' && stem[i] <= 'f'))) return false;
    }
    const std::string ext = name.substr(dot);
    if (ext != ".bmp" && ext != ".part") return false;
    if (suffix != nullptr) *suffix = ext;
    if (digest != nullptr) *digest = stem;
    return true;
}

StoreStatus Store::GarbageCollect() {
    if (fs_ == nullptr) return StoreStatus::kInvalidArgument;
    SlotRecord a, b;
    if (ReadSlot(0, &a) != StoreStatus::kOk || ReadSlot(1, &b) != StoreStatus::kOk ||
        !a.structural_valid || !b.structural_valid || !a.complete || !b.complete ||
        !SameSnapshot(a.snapshot, b.snapshot)) return StoreStatus::kNotReady;
    std::vector<std::string> paths;
    if (!fs_->ListFiles(kManagedDirectory, &paths)) return StoreStatus::kIoError;
    std::vector<std::string> keep;
    keep.reserve(a.snapshot.manifest.photos.size());
    for (size_t i = 0; i < a.snapshot.manifest.photos.size(); ++i) keep.push_back(a.snapshot.manifest.photos[i].sha256);
    for (size_t i = 0; i < paths.size(); ++i) {
        std::string suffix, digest;
        if (!IsManagedImageName(paths[i], &suffix, &digest)) continue;  // Preserve unknown files.
        if (suffix == ".part" || std::find(keep.begin(), keep.end(), digest) == keep.end()) {
            if (!fs_->Remove(paths[i])) return StoreStatus::kIoError;
        }
    }
    // Snapshot writes use fixed per-slot temporary names, so they are not
    // content-addressed and intentionally do not pass IsManagedImageName.
    // Remove only these two exact service-owned paths after both slots have
    // been verified; similarly named user files remain untouched.
    for (int slot = 0; slot != 2; ++slot) {
        size_t temp_size = 0;
        const std::string temp = SlotTempPath(slot);
        if (fs_->StatFile(temp, &temp_size) && !fs_->Remove(temp)) return StoreStatus::kIoError;
    }
    if (!fs_->SyncDirectory(kManagedDirectory)) return StoreStatus::kIoError;
    return StoreStatus::kOk;
}

const char* Store::StatusString(StoreStatus status) {
    switch (status) {
        case StoreStatus::kOk: return "ok";
        case StoreStatus::kNoSnapshot: return "no_snapshot";
        case StoreStatus::kInvalidArgument: return "invalid_argument";
        case StoreStatus::kIoError: return "io_error";
        case StoreStatus::kNotFound: return "not_found";
        case StoreStatus::kAlreadyExists: return "already_exists";
        case StoreStatus::kIntegrityError: return "integrity_error";
        case StoreStatus::kImageInvalid: return "image_invalid";
        case StoreStatus::kVersionRejected: return "version_rejected";
        case StoreStatus::kSameRevisionConflict: return "same_revision_conflict";
        case StoreStatus::kMirrorPending: return "mirror_pending";
        case StoreStatus::kNotReady: return "not_ready";
        case StoreStatus::kNoCompleteSnapshot: return "no_complete_snapshot";
        case StoreStatus::kBusy: return "busy";
    }
    return "unknown";
}

}  // namespace photopull
