// Host-only storage and protocol boundary tests.  Build by compiling this
// file with core.cpp and store.cpp, using -Icomponents/photopull.

#include "../core.h"
#include "../store.h"

#include <assert.h>
#include <stdint.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

using namespace photopull;

namespace {

enum class FaultOp { kNone, kWrite, kSync, kRename, kRemove, kSyncDirectory };

class FaultFs final : public FileSystem {
public:
    std::map<std::string, std::vector<uint8_t> > files;
    FaultOp fail_op = FaultOp::kNone;
    bool fail_after_mutation = false;
    bool failed = false;

    bool ReadFile(const std::string& path, std::vector<uint8_t>* data) override {
        if (data == nullptr) return false;
        const std::map<std::string, std::vector<uint8_t> >::const_iterator it = files.find(path);
        if (it == files.end()) return false;
        *data = it->second;
        return true;
    }

    bool ReadFileChunks(const std::string& path, ChunkCallback callback, void* context) override {
        if (callback == nullptr) return false;
        const std::map<std::string, std::vector<uint8_t> >::const_iterator it = files.find(path);
        if (it == files.end()) return false;
        const std::vector<uint8_t>& data = it->second;
        for (size_t offset = 0; offset < data.size();) {
            const size_t length = std::min<size_t>(4096, data.size() - offset);
            if (!callback(data.data() + offset, length, context)) return false;
            offset += length;
        }
        return true;
    }

    bool WriteFile(const std::string& path, const uint8_t* data, size_t length,
                   bool append, size_t* bytes_written) override {
        if (bytes_written == nullptr || (data == nullptr && length != 0)) return false;
        *bytes_written = 0;
        std::vector<uint8_t>& target = files[path];
        if (!append) target.clear();
        if (ShouldFail(FaultOp::kWrite)) {
            // Model a power cut in the middle of a media write.  The caller
            // must observe a short write and never publish this file.
            const size_t partial = length == 0 ? 0 : std::max<size_t>(1, length / 2);
            target.insert(target.end(), data, data + partial);
            *bytes_written = partial;
            return false;
        }
        target.insert(target.end(), data, data + length);
        *bytes_written = length;
        return true;
    }

    bool SyncFile(const std::string&) override {
        return !ShouldFail(FaultOp::kSync);
    }

    bool SyncDirectory(const std::string&) override {
        return !ShouldFail(FaultOp::kSyncDirectory);
    }

    bool Rename(const std::string& from, const std::string& to) override {
        if (ShouldFail(FaultOp::kRename)) {
            if (fail_after_mutation) {
                const std::map<std::string, std::vector<uint8_t> >::iterator it = files.find(from);
                if (it != files.end()) {
                    files[to] = it->second;
                    files.erase(it);
                }
            }
            return false;
        }
        const std::map<std::string, std::vector<uint8_t> >::iterator it = files.find(from);
        if (it == files.end()) return false;
        files[to] = it->second;
        files.erase(it);
        return true;
    }

    bool Remove(const std::string& path) override {
        if (ShouldFail(FaultOp::kRemove)) {
            if (fail_after_mutation) files.erase(path);
            return false;
        }
        files.erase(path);
        return true;
    }

    bool StatFile(const std::string& path, size_t* size) override {
        const std::map<std::string, std::vector<uint8_t> >::const_iterator it = files.find(path);
        if (it == files.end()) return false;
        if (size != nullptr) *size = it->second.size();
        return true;
    }

    bool ListFiles(const std::string&, std::vector<std::string>* paths) override {
        if (paths == nullptr) return false;
        paths->clear();
        // Include deliberately out-of-namespace paths to make GC's path
        // check observable even when an adapter returns a malformed listing.
        for (std::map<std::string, std::vector<uint8_t> >::const_iterator it = files.begin();
             it != files.end(); ++it) paths->push_back(it->first);
        return true;
    }

    void FailNext(FaultOp op, bool after_mutation = false) {
        fail_op = op;
        fail_after_mutation = after_mutation;
        failed = false;
    }

private:
    bool ShouldFail(FaultOp op) {
        if (failed || fail_op != op) return false;
        failed = true;
        return true;
    }
};

const char kShaZero[] = "a4c428ff67ad2eb1d67c40cee495a0a5d613ac234657a9fa778d3ee740721aca";
const char kShaOne[] = "e496791c49fa08b267021e8e5ddb336bd4a644fbb399de4e8e6931fe5c071729";
const char kShaFF[] = "107ffc794338d9c5ea0eb6cc244c71f4b4080a74f38628159939cd1cb072c0d9";
const char kShaBmp[] = "59e9925fe620459820bc1a92444c9e2ba12c745b15e9f111161f95d1c601d3ae";

std::string PhotoJson(const std::string& id, const std::string& path,
                      const std::string& sha) {
    return std::string("{\"id\":\"") + id + "\",\"path\":\"" + path +
           "\",\"size\":1152054,\"sha256\":\"" + sha +
           "\",\"width\":800,\"height\":480,\"format\":\"bmp24-6color\"}";
}

Manifest ParseManifest(uint32_t revision, const std::string& generation,
                       const std::vector<PhotoEntry>& photos) {
    std::string json = "{\"schema\":1,\"generation\":\"" + generation +
                       "\",\"revision\":" + std::to_string(revision) +
                       ",\"photos\":[";
    for (size_t i = 0; i < photos.size(); ++i) {
        if (i != 0) json.push_back(',');
        json += PhotoJson(photos[i].id, photos[i].path, photos[i].sha256);
    }
    json += "]}";
    Manifest manifest;
    assert(ParseManifestJson(json.data(), json.size(), &manifest).ok());
    return manifest;
}

Manifest OnePhoto(uint32_t revision, const char* sha, const char* id = "a",
                  const char* generation = "g") {
    PhotoEntry photo;
    photo.id = id;
    photo.path = std::string("/") + id + ".bmp";
    photo.sha256 = sha;
    photo.size = kExpectedBmpSize;
    photo.width = 800;
    photo.height = 480;
    photo.format = "bmp24-6color";
    std::vector<PhotoEntry> photos(1, photo);
    return ParseManifest(revision, generation, photos);
}

std::vector<uint8_t> Bytes(uint8_t value) {
    return std::vector<uint8_t>(kExpectedBmpSize, value);
}

std::vector<uint8_t> BlackBmp() {
    const size_t pixel_bytes = 800U * 480U * 3U;
    std::vector<uint8_t> bmp(kExpectedBmpSize, 0);
    bmp[0] = 'B';
    bmp[1] = 'M';
    const uint32_t total = static_cast<uint32_t>(bmp.size());
    bmp[2] = static_cast<uint8_t>(total);
    bmp[3] = static_cast<uint8_t>(total >> 8);
    bmp[4] = static_cast<uint8_t>(total >> 16);
    bmp[5] = static_cast<uint8_t>(total >> 24);
    bmp[10] = 54;
    bmp[14] = 40;
    bmp[18] = 0x20;
    bmp[19] = 0x03;
    bmp[22] = 0xE0;
    bmp[23] = 0x01;
    bmp[26] = 1;
    bmp[28] = 24;
    const uint32_t image = static_cast<uint32_t>(pixel_bytes);
    bmp[34] = static_cast<uint8_t>(image);
    bmp[35] = static_cast<uint8_t>(image >> 8);
    bmp[36] = static_cast<uint8_t>(image >> 16);
    bmp[37] = static_cast<uint8_t>(image >> 24);
    return bmp;
}

bool AcceptPhoto(FileSystem*, const std::string&, const PhotoEntry&, void*) { return true; }

struct ValidatorState {
    bool accept_bmp = true;
    size_t calls = 0;
};

bool StrictBmp(FileSystem* fs, const std::string& path, const PhotoEntry& expected,
               void* context) {
    ValidatorState* state = static_cast<ValidatorState*>(context);
    if (state == nullptr || fs == nullptr) return false;
    ++state->calls;
    if (!state->accept_bmp) return false;
    std::vector<uint8_t> bytes;
    if (!fs->ReadFile(path, &bytes) || bytes.size() != expected.size || bytes.size() < 54) return false;
    return bytes[0] == 'B' && bytes[1] == 'M' && bytes[10] == 54 && bytes[14] == 40 &&
           bytes[18] == 0x20 && bytes[19] == 0x03 && bytes[22] == 0xE0 && bytes[23] == 0x01 && bytes[26] == 1 &&
           bytes[28] == 24;
}

void WriteImage(Store* store, const PhotoEntry& photo, const std::vector<uint8_t>& bytes) {
    ImageWriter writer;
    assert(store->BeginImage(photo, &writer) == StoreStatus::kOk);
    for (size_t offset = 0; offset < bytes.size();) {
        const size_t length = std::min<size_t>(4096, bytes.size() - offset);
        assert(writer.Write(bytes.data() + offset, length) == StoreStatus::kOk);
        offset += length;
    }
    assert(writer.Finish() == StoreStatus::kOk);
}

void AssertComplete(const RecoveryState& recovery, size_t count) {
    assert(recovery.has_snapshot);
    assert(recovery.complete);
    assert(recovery.valid_images.size() == count);
    for (size_t i = 0; i < recovery.valid_images.size(); ++i) assert(recovery.valid_images[i]);
}

void EstablishV1(FaultFs* fs, Store* store, Manifest* manifest) {
    *manifest = OnePhoto(1, kShaZero);
    RecoveryState initial;
    assert(store->Recover(&initial) == StoreStatus::kNoSnapshot);
    WriteImage(store, manifest->photos[0], Bytes(0));
    const CommitResult committed = store->Commit(*manifest, "https://example.com/manifest.json", "old");
    assert(committed.committed && committed.mirrored && committed.status == StoreStatus::kOk);
    (void)fs;
}

void TestCommitFaults() {
    const FaultOp ops[] = {FaultOp::kWrite, FaultOp::kSync, FaultOp::kRename,
                           FaultOp::kRemove, FaultOp::kSyncDirectory};
    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i) {
        for (int after = 0; after != 2; ++after) {
            FaultFs fs;
            Store store(&fs, AcceptPhoto);
            Manifest first;
            EstablishV1(&fs, &store, &first);
            const Manifest second = OnePhoto(2, kShaOne);
            WriteImage(&store, second.photos[0], Bytes(1));
            fs.FailNext(ops[i], after != 0);
            store.Commit(second, "https://example.com/manifest.json", "new");

            // Reconstructing the manager models a reset at every write,
            // sync, rename, remove, and directory-sync boundary.  At least
            // one complete authoritative snapshot must still be playable.
            Store reboot(&fs, AcceptPhoto);
            RecoveryState recovery;
            const StoreStatus status = reboot.Recover(&recovery);
            assert(status == StoreStatus::kOk);
            AssertComplete(recovery, 1);
            assert(recovery.snapshot.etag == "old" || recovery.snapshot.etag == "new");
        }
    }
}

void TestDeferredMirrorRevalidatesImages() {
    FaultFs fs;
    Store store(&fs, AcceptPhoto);
    Manifest first;
    EstablishV1(&fs, &store, &first);
    const Manifest second = OnePhoto(2, kShaOne);
    WriteImage(&store, second.photos[0], Bytes(1));
    const CommitResult pending = store.Commit(second, "https://example.com/manifest.json", "new", true);
    assert(pending.committed && !pending.mirrored && pending.status == StoreStatus::kMirrorPending);

    fs.files.erase(ManagedImagePath(kShaOne));
    bool mirrored = true;
    bool gc_pending = false;
    assert(store.RepairMirror(&mirrored, &gc_pending) == StoreStatus::kNoCompleteSnapshot);
    assert(!mirrored);
    Store reboot(&fs, AcceptPhoto);
    RecoveryState damaged;
    // The previous slot is still complete, so recovery keeps it for offline
    // playback while retaining the newer damaged slot as repair metadata.
    assert(reboot.Recover(&damaged) == StoreStatus::kOk);
    assert(damaged.has_snapshot && damaged.complete && damaged.snapshot.manifest.revision == 1);
    assert(damaged.has_newer_incomplete && damaged.newer_incomplete_snapshot.manifest.revision == 2);
    assert(!damaged.mirror_pending && damaged.gc_pending);
    assert(fs.files.count(ManagedImagePath(kShaZero)) == 1);

    WriteImage(&reboot, second.photos[0], Bytes(1));
    const CommitResult repaired = reboot.Commit(second, "https://example.com/manifest.json", "new", true);
    assert(repaired.committed && !repaired.mirrored);
    assert(reboot.RepairMirror(&mirrored, &gc_pending) == StoreStatus::kOk);
    assert(mirrored && !gc_pending);
    assert(fs.files.count(ManagedImagePath(kShaOne)) == 1);
    assert(fs.files.count(ManagedImagePath(kShaZero)) == 0);
}

void TestCompleteFallbackPreservesRepairBasis() {
    FaultFs fs;
    Store store(&fs, AcceptPhoto);
    Manifest old_manifest;
    EstablishV1(&fs, &store, &old_manifest);

    const Manifest newer = OnePhoto(2, kShaOne);
    WriteImage(&store, newer.photos[0], Bytes(1));
    const CommitResult pending = store.Commit(newer, "https://example.com/manifest.json", "new", true);
    assert(pending.committed && !pending.mirrored && pending.status == StoreStatus::kMirrorPending);
    assert(fs.files.count(ManagedImagePath(kShaOne)) == 1);

    // The newest record remains parseable, but its content is damaged after
    // the reset.  Recovery must play the older complete snapshot and retain
    // the newer revision as the basis for a later repair.
    fs.files.erase(ManagedImagePath(kShaOne));
    Store reboot(&fs, AcceptPhoto);
    RecoveryState recovered;
    assert(reboot.Recover(&recovered) == StoreStatus::kOk);
    assert(recovered.complete && recovered.snapshot.manifest.revision == 1);
    assert(recovered.active_slot == 0);
    assert(recovered.has_newer_incomplete);
    assert(recovered.newer_incomplete_slot == 1);
    assert(recovered.newer_incomplete_snapshot.manifest.revision == 2);
    assert(recovered.newer_incomplete_valid_images.size() == 1 &&
           !recovered.newer_incomplete_valid_images[0]);
    assert(!recovered.mirror_pending && recovered.gc_pending);

    // The damaged newer revision still prevents an older candidate from
    // being accepted, while the same revision is explicitly repairable.
    CandidatePlan rollback;
    assert(reboot.PlanCandidate(old_manifest, "https://example.com/manifest.json", &rollback) ==
           StoreStatus::kVersionRejected);
    CandidatePlan repair;
    assert(reboot.PlanCandidate(newer, "https://example.com/manifest.json", &repair) == StoreStatus::kOk);
    assert(repair.decision == VersionDecision::kNotModified);
    assert(repair.needs_repair && repair.total_missing_bytes == kExpectedBmpSize);

    // Neither mirror repair nor direct GC may overwrite the newer record or
    // remove the old complete image before content repair has succeeded.
    bool mirrored = false;
    bool gc_pending = false;
    assert(reboot.RepairMirror(&mirrored, &gc_pending) == StoreStatus::kMirrorPending);
    assert(!mirrored && gc_pending);
    assert(reboot.GarbageCollect() == StoreStatus::kNotReady);
    assert(fs.files.count(ManagedImagePath(kShaZero)) == 1);
    assert(fs.files.count(std::string(kManagedDirectory) + "/snapshot_b.bin") == 1);

    WriteImage(&reboot, newer.photos[0], Bytes(1));
    const CommitResult repaired = reboot.Commit(newer, "https://example.com/manifest.json", "new", true);
    assert(repaired.committed && !repaired.mirrored && repaired.status == StoreStatus::kMirrorPending);
    assert(reboot.RepairMirror(&mirrored, &gc_pending) == StoreStatus::kOk);
    assert(mirrored && !gc_pending);
    assert(fs.files.count(ManagedImagePath(kShaZero)) == 0);
    assert(fs.files.count(ManagedImagePath(kShaOne)) == 1);
    assert(reboot.Recover(&recovered) == StoreStatus::kOk);
    assert(recovered.complete && recovered.snapshot.manifest.revision == 2);
    assert(!recovered.has_newer_incomplete && !recovered.mirror_pending);
}

void TestEtagAndNamespaceIsolation() {
    FaultFs fs;
    Store store(&fs, AcceptPhoto);
    Manifest first;
    EstablishV1(&fs, &store, &first);
    const Manifest second = OnePhoto(2, kShaOne);
    WriteImage(&store, second.photos[0], Bytes(1));

    // A candidate ETag is not durable until its snapshot passes all checks.
    const CommitResult bad = store.Commit(second, "https://example.com/manifest.json", "bad\n");
    assert(!bad.committed);
    RecoveryState old;
    assert(store.Recover(&old) == StoreStatus::kOk);
    assert(old.snapshot.etag == "old");

    const std::string orphan = std::string(kManagedDirectory) + "/" +
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.bmp";
    const std::string outside = std::string("/sdcard/other/") +
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.bmp";
    const std::string unknown = std::string(kManagedDirectory) + "/keep.me";
    const std::string slot_temp_a = std::string(kManagedDirectory) + "/snapshot_a.tmp";
    const std::string slot_temp_b = std::string(kManagedDirectory) + "/snapshot_b.tmp";
    fs.files[orphan] = Bytes(0);
    fs.files[outside] = Bytes(0);
    fs.files[unknown] = Bytes(0);
    fs.files[slot_temp_a] = Bytes(0);
    fs.files[slot_temp_b] = Bytes(0);
    const CommitResult good = store.Commit(second, "https://example.com/manifest.json", "new");
    assert(good.committed && good.mirrored && good.status == StoreStatus::kOk);
    assert(fs.files.count(orphan) == 0);
    assert(fs.files.count(slot_temp_a) == 0 && fs.files.count(slot_temp_b) == 0);
    assert(fs.files.count(outside) == 1);
    assert(fs.files.count(unknown) == 1);
}

void TestStrictValidator() {
    FaultFs fs;
    ValidatorState validator;
    Store store(&fs, StrictBmp, &validator);
    const Manifest manifest = OnePhoto(1, kShaBmp);
    const std::vector<uint8_t> bmp = BlackBmp();
    WriteImage(&store, manifest.photos[0], bmp);
    assert(validator.calls != 0);
    const CommitResult committed = store.Commit(manifest, "https://example.com/manifest.json", "bmp");
    assert(committed.committed && committed.mirrored);
    Store reboot(&fs, StrictBmp, &validator);
    RecoveryState recovered;
    assert(reboot.Recover(&recovered) == StoreStatus::kOk);
    AssertComplete(recovered, 1);

    validator.accept_bmp = false;
    const Manifest rejected = OnePhoto(2, kShaZero);
    ImageWriter writer;
    assert(reboot.BeginImage(rejected.photos[0], &writer) == StoreStatus::kOk);
    const std::vector<uint8_t> bad = Bytes(0);
    for (size_t offset = 0; offset < bad.size();) {
        const size_t length = std::min<size_t>(4096, bad.size() - offset);
        assert(writer.Write(bad.data() + offset, length) == StoreStatus::kOk);
        offset += length;
    }
    assert(writer.Finish() == StoreStatus::kImageInvalid);
    assert(fs.files.count(ManagedImagePath(kShaZero)) == 0);
}

void TestFiftyPhotosAndDuplicateSha() {
    std::vector<PhotoEntry> photos;
    for (size_t i = 0; i < kMaxPhotos; ++i) {
        PhotoEntry photo;
        photo.id = "photo-" + std::to_string(i);
        photo.path = "/ordered/" + std::to_string(i) + ".bmp";
        // Three content files serve 50 ordered IDs, proving the storage
        // planner counts content-addressed bytes once per SHA.
        photo.sha256 = i % 3 == 0 ? kShaZero : (i % 3 == 1 ? kShaOne : kShaFF);
        photo.size = kExpectedBmpSize;
        photo.width = 800;
        photo.height = 480;
        photo.format = "bmp24-6color";
        photos.push_back(photo);
    }
    const Manifest manifest = ParseManifest(7, "fifty", photos);
    assert(manifest.photos.size() == kMaxPhotos);
    assert(manifest.photos.front().id == "photo-0");
    assert(manifest.photos.back().id == "photo-49");
    assert(manifest.normalized_json.size() < kManifestMaxBytes);

    FaultFs fs;
    Store store(&fs, AcceptPhoto);
    CandidatePlan plan;
    assert(store.PlanCandidate(manifest, "https://EXAMPLE.com:443/manifest.json", &plan) == StoreStatus::kOk);
    assert(plan.total_missing_bytes == 3U * kExpectedBmpSize);
    WriteImage(&store, manifest.photos[0], Bytes(0));
    WriteImage(&store, manifest.photos[1], Bytes(1));
    WriteImage(&store, manifest.photos[2], Bytes(255));
    const CommitResult committed = store.Commit(manifest, "https://EXAMPLE.com:443/manifest.json", "fifty");
    assert(committed.committed && committed.mirrored && committed.status == StoreStatus::kOk);
    RecoveryState recovered;
    assert(store.Recover(&recovered) == StoreStatus::kOk);
    AssertComplete(recovered, kMaxPhotos);
    assert(recovered.snapshot.source == "https://example.com/manifest.json");
    assert(recovered.snapshot.manifest.photos.back().id == "photo-49");

    std::vector<PhotoEntry> reversed = photos;
    std::reverse(reversed.begin(), reversed.end());
    const Manifest reordered = ParseManifest(7, "fifty", reversed);
    assert(CompareManifest(reordered, &manifest, true) == VersionDecision::kRejectSameRevisionConflict);
    const Manifest empty = ParseManifest(8, "fifty", std::vector<PhotoEntry>());
    assert(empty.photos.empty());
    // Empty is a real version, not the recovery representation for damage.
    const CommitResult empty_commit = store.Commit(empty, "https://example.com/manifest.json", "empty");
    assert(empty_commit.committed && empty_commit.mirrored);
    assert(store.Recover(&recovered) == StoreStatus::kOk);
    assert(recovered.complete && recovered.valid_images.empty());
}

std::string ConfigJson(const std::string& ssid, const std::string& password,
                       const std::string& token, uint32_t poll, uint32_t display,
                       const std::string& base_url, const std::string& path) {
    return std::string("{\"schema\":1,\"enabled\":true,\"wifi\":{\"ssid\":\"") + ssid +
           "\",\"password\":\"" + password + "\"},\"server\":{\"base_url\":\"" +
           base_url + "\",\"manifest_path\":\"" + path +
           "\",\"bearer_token\":\"" + token + "\",\"ca_file\":\"/sdcard/ca.pem\"}," +
           "\"poll_interval_sec\":" + std::to_string(poll) +
           ",\"default_display_interval_sec\":" + std::to_string(display) + "}";
}

void TestConfigBoundaries() {
    PhotoPullConfig config;
    const std::string host(504, 'a');  // https:// + 504 bytes = 512 bytes.
    const std::string path = "/" + std::string(255, 'p');
    const std::string valid = ConfigJson(std::string(32, 's'), std::string(64, 'p'),
                                         std::string(2048, 't'), kMinIntervalSec,
                                         kMaxIntervalSec, "https://" + host, path);
    assert(ParseConfigJson(valid.data(), valid.size(), &config).ok());
    assert(config.ssid.size() == 32 && config.password.size() == 64 && config.bearer_token.size() == 2048);
    assert(config.base_url == "https://" + host);
    assert(config.manifest_path.size() == kMaxPhotoPathBytes);

    for (uint32_t value = 59; value != 86402; ++value) {
        if (value != 59 && value != 86401) continue;
        const std::string json = ConfigJson("s", "p", "", value, kDefaultDisplayIntervalSec,
                                            "https://example.com", "/manifest.json");
        assert(!ParseConfigJson(json.data(), json.size(), &config).ok());
    }
    const std::string low_display = ConfigJson("s", "p", "", kMinIntervalSec, 59,
                                               "https://example.com", "/manifest.json");
    const std::string high_display = ConfigJson("s", "p", "", kMinIntervalSec, 86401,
                                                "https://example.com", "/manifest.json");
    assert(!ParseConfigJson(low_display.data(), low_display.size(), &config).ok());
    assert(!ParseConfigJson(high_display.data(), high_display.size(), &config).ok());
    assert(!ParseConfigJson(ConfigJson(std::string(33, 's'), "p", "", 60, 60,
                                       "https://example.com", "/manifest.json").data(),
                            ConfigJson(std::string(33, 's'), "p", "", 60, 60,
                                       "https://example.com", "/manifest.json").size(), &config).ok());
    assert(!ParseConfigJson(ConfigJson("s", std::string(65, 'p'), "", 60, 60,
                                       "https://example.com", "/manifest.json").data(),
                            ConfigJson("s", std::string(65, 'p'), "", 60, 60,
                                       "https://example.com", "/manifest.json").size(), &config).ok());
    const std::string no_server = "{\"schema\":1,\"enabled\":false}";
    assert(ParseConfigJson(no_server.data(), no_server.size(), &config).ok());

    std::string exact = ConfigJson("s", "p", "", 60, 60, "https://example.com", "/manifest.json");
    exact.append(kConfigMaxBytes - exact.size(), ' ');
    assert(exact.size() == kConfigMaxBytes);
    assert(ParseConfigJson(exact.data(), exact.size(), &config).ok());
    exact.push_back(' ');
    assert(!ParseConfigJson(exact.data(), exact.size(), &config).ok());
}

void TestShaVectorsAndSource() {
    assert(IsValidManagedSha256(kShaZero));
    assert(IsValidManagedSha256(kShaOne));
    assert(IsValidManagedSha256(kShaFF));
    assert(!IsValidManagedSha256(std::string(63, '0')));
    FaultFs fs;
    Store store(&fs, AcceptPhoto);
    const char* shas[] = {kShaZero, kShaOne, kShaFF};
    const uint8_t values[] = {0, 1, 255};
    for (size_t i = 0; i < 3; ++i) {
        const Manifest manifest = OnePhoto(static_cast<uint32_t>(i + 1), shas[i]);
        WriteImage(&store, manifest.photos[0], Bytes(values[i]));
        const CommitResult result = store.Commit(manifest, "https://example.com/manifest.json",
                                                 std::string("v") + std::to_string(i));
        assert(result.committed);
    }
}

}  // namespace

int main() {
    std::string source;
    assert(NormalizeSourceUrl("https://EXAMPLE.com:443/manifest.json", &source));
    assert(source == "https://example.com/manifest.json");
    assert(!NormalizeSourceUrl("http://example.com/manifest.json", &source));
    assert(!NormalizeSourceUrl("https://example.com/a%2fb", &source));
    assert(!NormalizeSourceUrl("https://example.com/../manifest.json", &source));
    TestShaVectorsAndSource();
    TestConfigBoundaries();
    TestFiftyPhotosAndDuplicateSha();
    TestStrictValidator();
    TestEtagAndNamespaceIsolation();
    TestDeferredMirrorRevalidatesImages();
    TestCompleteFallbackPreservesRepairBasis();
    TestCommitFaults();
    return 0;
}
