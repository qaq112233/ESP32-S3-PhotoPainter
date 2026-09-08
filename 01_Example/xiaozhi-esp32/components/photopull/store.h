#pragma once

#include "core.h"

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace photopull {

// The firmware adapter can map these calls to FatFS, while host tests can use
// an in-memory implementation with deterministic failure injection.  A true
// WriteFile result means all bytes were written; bytes_written makes short
// writes observable and prevents a partially written part/snapshot from being
// mistaken for a durable file.
class FileSystem {
public:
    typedef bool (*ChunkCallback)(const uint8_t* data, size_t length, void* context);

    virtual ~FileSystem() {}
    virtual bool ReadFile(const std::string& path, std::vector<uint8_t>* data) = 0;
    virtual bool ReadFileChunks(const std::string& path, ChunkCallback callback,
                                void* context) = 0;
    virtual bool WriteFile(const std::string& path, const uint8_t* data,
                           size_t length, bool append, size_t* bytes_written) = 0;
    virtual bool SyncFile(const std::string& path) = 0;
    virtual bool SyncDirectory(const std::string& path) = 0;
    virtual bool Rename(const std::string& from, const std::string& to) = 0;
    virtual bool Remove(const std::string& path) = 0;
    virtual bool StatFile(const std::string& path, size_t* size) = 0;
    virtual bool ListFiles(const std::string& directory,
                           std::vector<std::string>* paths) = 0;
};

// This callback is owned by the display/image-validation component.  It must
// validate the canonical 24-bit BMP structure and six-colour pixel rule.  The
// callback runs before a .part file is published as a SHA-named image.
typedef bool (*PhotoValidator)(FileSystem* fs, const std::string& path,
                               const PhotoEntry& expected, void* context);

enum class StoreStatus {
    kOk = 0,
    kNoSnapshot,
    kInvalidArgument,
    kIoError,
    kNotFound,
    kAlreadyExists,
    kIntegrityError,
    kImageInvalid,
    kVersionRejected,
    kSameRevisionConflict,
    kMirrorPending,
    kNotReady,
    kNoCompleteSnapshot,
    kBusy,
};

struct SnapshotInfo {
    uint64_t commit_seq = 0;
    std::string source;
    std::string etag;
    Manifest manifest;
    uint8_t payload_sha256[32] = {0};
};

struct RecoveryState {
    bool has_snapshot = false;
    bool complete = false;
    bool mirror_pending = false;
    bool gc_pending = false;
    // If the newest structurally valid slot is incomplete while an older
    // complete slot exists, keep the newer record as a repair/version basis.
    // It is not used for offline playback until all of its images validate.
    bool has_newer_incomplete = false;
    int newer_incomplete_slot = -1;
    SnapshotInfo newer_incomplete_snapshot;
    std::vector<bool> newer_incomplete_valid_images;
    int active_slot = -1;  // 0 = snapshot_a, 1 = snapshot_b.
    SnapshotInfo snapshot;
    std::vector<bool> valid_images;
};

struct MissingPhoto {
    PhotoEntry photo;
    bool present = false;
    bool valid = false;
};

struct CandidatePlan {
    VersionDecision decision = VersionDecision::kAccept;
    bool needs_repair = false;
    size_t total_missing_bytes = 0;
    std::vector<MissingPhoto> photos;
};

struct CommitResult {
    StoreStatus status = StoreStatus::kInvalidArgument;
    uint64_t commit_seq = 0;
    bool committed = false;  // New snapshot durable, or identical library accepted as a no-op.
    bool mirrored = false;   // Both slots point to that complete snapshot.
    bool gc_pending = false;
};

class Store;

class ImageWriter {
public:
    ImageWriter();
    ImageWriter(const ImageWriter&) = delete;
    ImageWriter& operator=(const ImageWriter&) = delete;
    ImageWriter(ImageWriter&& other);
    ImageWriter& operator=(ImageWriter&& other);
    ~ImageWriter();

    StoreStatus Write(const uint8_t* data, size_t length);
    StoreStatus Finish();
    void Abort();
    bool open() const { return store_ != nullptr; }

private:
    friend class Store;
    Store* store_;
    PhotoEntry expected_;
    std::string part_path_;
    uint64_t bytes_written_;
    struct HashState* hash_;
    bool finished_;
};

class Store {
public:
    Store(FileSystem* fs, PhotoValidator validator = nullptr,
          void* validator_context = nullptr,
          uint32_t default_display_interval_sec = kDefaultDisplayIntervalSec);
    ~Store();

    StoreStatus Recover(RecoveryState* out);
    const RecoveryState& recovery() const { return recovery_; }

    // Compare a candidate to the latest locally selected snapshot and inspect
    // every referenced content-addressed image.  A structurally valid but
    // incomplete snapshot is treated as a repair target, so a repeated server
    // revision can still redownload its missing/corrupt files.
    StoreStatus PlanCandidate(const Manifest& candidate, const std::string& source,
                              CandidatePlan* out);

    // Begin/stream/finish one missing image.  Finish checks exact byte count,
    // SHA-256, the image validator, fsync and atomic publication before it
    // returns success.  The service can feed bounded HTTPS chunks directly to
    // Write without retaining the image in RAM.
    StoreStatus BeginImage(const PhotoEntry& expected, ImageWriter* out);

    // The first slot write is the logical commit point.  Mirroring and cleanup
    // are attempted afterwards; a mirror failure returns kMirrorPending while
    // leaving committed=true and preserving the old files.
    CommitResult Commit(const Manifest& manifest, const std::string& source,
                        const std::string& etag, bool defer_mirror = false);

    // Retry a previously failed mirror.  GC runs only after both slots have
    // been verified to reference the same complete snapshot.
    StoreStatus RepairMirror(bool* mirrored, bool* gc_pending);
    StoreStatus GarbageCollect();

    StoreStatus CheckPhoto(const PhotoEntry& expected, bool* valid);
    StoreStatus ValidateSnapshot(const std::string& path, SnapshotInfo* out);

    static const char* StatusString(StoreStatus status);

private:
    friend class ImageWriter;

    struct SlotRecord {
        bool structural_valid = false;
        bool complete = false;
        SnapshotInfo snapshot;
        std::vector<bool> valid_images;
    };

    StoreStatus FinishImage(ImageWriter* writer);
    StoreStatus WriteImagePart(const ImageWriter* writer, const uint8_t* data,
                               size_t length, bool append, size_t* bytes_written);
    StoreStatus WriteSnapshotToSlot(const std::vector<uint8_t>& bytes, int slot,
                                    SnapshotInfo* verified);
    StoreStatus MirrorSnapshot(const std::vector<uint8_t>& bytes, int slot,
                               SnapshotInfo* verified);
    StoreStatus BuildSnapshot(const Manifest& manifest, const std::string& source,
                              const std::string& etag, uint64_t commit_seq,
                              std::vector<uint8_t>* bytes);
    StoreStatus ParseSnapshotBytes(const std::vector<uint8_t>& bytes,
                                   SnapshotInfo* out);
    StoreStatus ReadSlot(int slot, SlotRecord* out);
    StoreStatus ValidateSnapshotImages(const SnapshotInfo& snapshot,
                                       std::vector<bool>* valid, bool* complete);
    StoreStatus ValidatePhotoInternal(const PhotoEntry& expected, bool* valid);
    StoreStatus EnsureDirectory();
    std::string SlotPath(int slot) const;
    std::string SlotTempPath(int slot) const;
    static bool SameSnapshot(const SnapshotInfo& a, const SnapshotInfo& b);
    static bool IsManagedImageName(const std::string& path, std::string* suffix,
                                   std::string* digest);

    FileSystem* fs_;
    PhotoValidator validator_;
    void* validator_context_;
    uint32_t default_display_interval_sec_;
    RecoveryState recovery_;
};

}  // namespace photopull
