#pragma once
#include "store.h"
namespace photopull {
class FatFsStore final : public FileSystem {
public:
    void SetProgress(void (*callback)(void*), void* context) { progress_ = callback; context_ = context; }
    bool ReadFile(const std::string&, std::vector<uint8_t>*) override;
    bool ReadFileChunks(const std::string&, ChunkCallback, void*) override;
    bool WriteFile(const std::string&, const uint8_t*, size_t, bool, size_t*) override;
    bool SyncFile(const std::string&) override;
    bool SyncDirectory(const std::string&) override;
    bool Rename(const std::string&, const std::string&) override;
    bool Remove(const std::string&) override;
    bool StatFile(const std::string&, size_t*) override;
    bool ListFiles(const std::string&, std::vector<std::string>*) override;
private:
    void Progress() { if (progress_ && !inside_progress_) { inside_progress_ = true; progress_(context_); inside_progress_ = false; } }
    void (*progress_)(void*) = nullptr;
    void* context_ = nullptr;
    bool inside_progress_ = false;
};
bool ValidateStoredBmp(FileSystem*, const std::string&, const PhotoEntry&, void*);
}
