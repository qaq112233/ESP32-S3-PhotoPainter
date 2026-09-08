// Host-only fault-injection tests for the storage core.
// Build manually by compiling core.cpp, store.cpp and this file with C++11.

#include "../core.h"
#include "../store.h"

#include <assert.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

using namespace photopull;

class MemFs : public FileSystem {
public:
    std::map<std::string, std::vector<uint8_t> > files;
    int fail_write_calls = 0;
    int fail_rename_calls = 0;
    int fail_sync_calls = 0;
    int fail_remove_calls = 0;
    size_t short_write = 0;
    int fail_on_rename_number = 0;
    int rename_attempts = 0;

    bool ReadFile(const std::string& path, std::vector<uint8_t>* data) override {
        std::map<std::string, std::vector<uint8_t> >::const_iterator it = files.find(path);
        if (it == files.end()) return false;
        *data = it->second;
        return true;
    }
    bool ReadFileChunks(const std::string& path, ChunkCallback callback, void* context) override {
        std::map<std::string, std::vector<uint8_t> >::const_iterator it = files.find(path);
        if (it == files.end() || callback == nullptr) return false;
        const std::vector<uint8_t>& data = it->second;
        for (size_t offset = 0; offset < data.size();) {
            const size_t length = (data.size() - offset > 4096) ? 4096 : data.size() - offset;
            if (!callback(data.data() + offset, length, context)) return false;
            offset += length;
        }
        return true;
    }
    bool WriteFile(const std::string& path, const uint8_t* data, size_t length,
                   bool append, size_t* bytes_written) override {
        if (bytes_written == nullptr) return false;
        *bytes_written = 0;
        if (fail_write_calls > 0) {
            --fail_write_calls;
            return false;
        }
        std::vector<uint8_t>& target = files[path];
        if (!append) target.clear();
        size_t count = length;
        if (short_write != 0 && count > short_write) count = short_write;
        if (count != 0) target.insert(target.end(), data, data + count);
        *bytes_written = count;
        return count == length;
    }
    bool SyncFile(const std::string&) override {
        if (fail_sync_calls > 0) { --fail_sync_calls; return false; }
        return true;
    }
    bool SyncDirectory(const std::string&) override { return true; }
    bool Rename(const std::string& from, const std::string& to) override {
        ++rename_attempts;
        if (fail_rename_calls > 0) { --fail_rename_calls; return false; }
        if (fail_on_rename_number != 0 && rename_attempts == fail_on_rename_number) return false;
        std::map<std::string, std::vector<uint8_t> >::iterator it = files.find(from);
        if (it == files.end()) return false;
        files[to] = it->second;
        files.erase(it);
        return true;
    }
    bool Remove(const std::string& path) override {
        if (fail_remove_calls > 0) { --fail_remove_calls; return false; }
        files.erase(path);
        return true;
    }
    bool StatFile(const std::string& path, size_t* size) override {
        std::map<std::string, std::vector<uint8_t> >::const_iterator it = files.find(path);
        if (it == files.end()) return false;
        if (size != nullptr) *size = it->second.size();
        return true;
    }
    bool ListFiles(const std::string&, std::vector<std::string>* paths) override {
        paths->clear();
        for (std::map<std::string, std::vector<uint8_t> >::const_iterator it = files.begin(); it != files.end(); ++it) {
            if (it->first.compare(0, strlen(kManagedDirectory) + 1, std::string(kManagedDirectory) + "/") == 0) paths->push_back(it->first);
        }
        return true;
    }
};

static bool AcceptPhoto(FileSystem*, const std::string&, const PhotoEntry&, void*) { return true; }

static Manifest MakeManifest(uint32_t revision, const char* sha) {
    const std::string json = std::string("{\"photos\":[{\"format\":\"bmp24-6color\",\"height\":480,\"sha256\":\"") + sha +
        "\",\"size\":1152054,\"path\":\"/a.bmp\",\"id\":\"a\",\"width\":800}],\"revision\":" +
        std::to_string(revision) + ",\"generation\":\"g\",\"schema\":1}";
    Manifest manifest;
    assert(ParseManifestJson(json.data(), json.size(), &manifest).ok());
    return manifest;
}

static void WriteImage(Store* store, const PhotoEntry& photo, uint8_t value) {
    ImageWriter writer;
    assert(store->BeginImage(photo, &writer) == StoreStatus::kOk);
    std::vector<uint8_t> bytes(4096, value);
    size_t remaining = photo.size;
    while (remaining != 0) {
        const size_t count = remaining < bytes.size() ? remaining : bytes.size();
        assert(writer.Write(bytes.data(), count) == StoreStatus::kOk);
        remaining -= count;
    }
    assert(writer.Finish() == StoreStatus::kOk);
}

int main() {
    const std::string sha = "a4c428ff67ad2eb1d67c40cee495a0a5d613ac234657a9fa778d3ee740721aca";
    Manifest manifest = MakeManifest(1, sha.c_str());
    assert(manifest.normalized_json.find("\"schema\":1") != std::string::npos);
    std::string normalized_source;
    assert(NormalizeSourceUrl("https://EXAMPLE.com:443/manifest.json", &normalized_source));
    assert(normalized_source == "https://example.com/manifest.json");
    assert(!NormalizeSourceUrl("http://example.com/manifest.json", &normalized_source));
    assert(!NormalizeSourceUrl("https://example.com/../manifest.json", &normalized_source));
    Manifest same;
    const std::string spaced = " { \"schema\": 1, \"generation\": \"g\", \"revision\": 1, \"photos\": [] } ";
    // An empty candidate is useful for parser tests, but it is a different
    // version from the one-photo manifest and must not be accepted as same rev.
    assert(ParseManifestJson(spaced.data(), spaced.size(), &same).ok());
    assert(CompareManifest(same, &manifest, true) == VersionDecision::kRejectSameRevisionConflict);

    PhotoPullConfig config;
    const std::string config_json = "{\"schema\":1,\"enabled\":true,\"wifi\":{\"ssid\":\"x\",\"password\":\"\"},\"server\":{\"base_url\":\"HTTPS://example.com/\",\"manifest_path\":\"/manifest.json\"}}";
    assert(!ParseConfigJson(config_json.data(), config_json.size(), &config).ok());
    const std::string valid_config = "{\"schema\":1,\"enabled\":true,\"wifi\":{\"ssid\":\"x\",\"password\":\"\"},\"server\":{\"base_url\":\"https://example.com/\",\"manifest_path\":\"/manifest.json\"}}";
    assert(ParseConfigJson(valid_config.data(), valid_config.size(), &config).ok());
    assert(config.base_url == "https://example.com");
    const std::string bad_url = "{\"schema\":1,\"enabled\":false,\"server\":{\"base_url\":\"https://example.com:0\",\"manifest_path\":\"/manifest.json\"}}";
    assert(!ParseConfigJson(bad_url.data(), bad_url.size(), &config).ok());
    const std::string trailing = "{\"schema\":1,\"enabled\":false,}";
    assert(!ParseConfigJson(trailing.data(), trailing.size(), &config).ok());

    MemFs fs;
    Store store(&fs, AcceptPhoto);
    RecoveryState state;
    assert(store.Recover(&state) == StoreStatus::kNoSnapshot);

    PhotoEntry photo = manifest.photos[0];
    WriteImage(&store, photo, 0);
    CommitResult first = store.Commit(manifest, "https://example.com/manifest.json", "v1");
    assert(first.committed && first.mirrored && first.status == StoreStatus::kOk);
    assert(fs.files.count(std::string(kManagedDirectory) + "/snapshot_a.bin") == 1);
    assert(fs.files.count(std::string(kManagedDirectory) + "/snapshot_b.bin") == 1);

    // A new revision is written into one slot, then mirrored.  An unrelated
    // SHA file and a user-named file demonstrate namespace-scoped cleanup.
    const std::string orphan = std::string(kManagedDirectory) + "/" +
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.bmp";
    fs.files[orphan] = std::vector<uint8_t>(1, 1);
    fs.files[std::string(kManagedDirectory) + "/keep.me"] = std::vector<uint8_t>(1, 1);
    const std::string sha_one = "e496791c49fa08b267021e8e5ddb336bd4a644fbb399de4e8e6931fe5c071729";
    Manifest second = MakeManifest(2, sha_one.c_str());
    WriteImage(&store, second.photos[0], 1);
    CommitResult committed = store.Commit(second, "https://example.com/manifest.json", "v2");
    assert(committed.committed && committed.mirrored && !committed.gc_pending);
    assert(fs.files.count(orphan) == 0);
    assert(fs.files.count(std::string(kManagedDirectory) + "/keep.me") == 1);
    assert(fs.files.count(ManagedImagePath(photo.sha256)) == 0);
    CommitResult no_change = store.Commit(second, "https://example.com/manifest.json", "v2");
    assert(no_change.committed && no_change.mirrored && no_change.commit_seq == committed.commit_seq);

    // The first snapshot write is durable, but the mirror fails.  The logical
    // commit remains available and RepairMirror completes it later.
    const std::string sha_ff = "107ffc794338d9c5ea0eb6cc244c71f4b4080a74f38628159939cd1cb072c0d9";
    Manifest third = MakeManifest(3, sha_ff.c_str());
    WriteImage(&store, third.photos[0], 255);
    fs.fail_on_rename_number = fs.rename_attempts + 2;
    CommitResult pending = store.Commit(third, "https://example.com/manifest.json", "v3");
    assert(pending.committed && !pending.mirrored && pending.status == StoreStatus::kMirrorPending);
    fs.fail_on_rename_number = 0;
    bool mirrored = false, gc_pending = false;
    assert(store.RepairMirror(&mirrored, &gc_pending) == StoreStatus::kOk);
    assert(mirrored && !gc_pending);

    // Removing a referenced image never turns the manifest into an empty
    // library.  Recovery reports the structural snapshot and a false mask.
    fs.files.erase(ManagedImagePath(sha_ff));
    Store recovered(&fs, AcceptPhoto);
    RecoveryState damaged;
    assert(recovered.Recover(&damaged) == StoreStatus::kNoCompleteSnapshot);
    assert(damaged.has_snapshot && !damaged.complete && damaged.valid_images.size() == 1 && !damaged.valid_images[0]);

    // Repairing the latest structurally valid but incomplete snapshot must be
    // allowed even though its mirror is pending.  This is the recovery path
    // used after an interrupted image publication or a later media error.
    WriteImage(&recovered, third.photos[0], 255);
    CommitResult repaired_commit = recovered.Commit(third, "https://example.com/manifest.json", "v3", true);
    assert(repaired_commit.committed && !repaired_commit.mirrored);
    bool repaired_mirror = false, repaired_gc = false;
    assert(recovered.RepairMirror(&repaired_mirror, &repaired_gc) == StoreStatus::kOk);
    assert(repaired_mirror && !repaired_gc);

    // A short snapshot write cannot become a commit.
    fs.short_write = 10;
    Manifest fourth = MakeManifest(4, sha_ff.c_str());
    CommitResult short_write = recovered.Commit(fourth, "https://example.com/manifest.json", "v4");
    assert(!short_write.committed);
    return 0;
}
