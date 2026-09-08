#include "fatfs_store.h"
#include "bmp_validator.h"
#include <cstdio>
#include <cerrno>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace photopull {
bool FatFsStore::ReadFile(const std::string& path, std::vector<uint8_t>* data) {
    size_t size;
    if (!data || !StatFile(path, &size) || size > 64 * 1024) return false;
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) return false;
    data->resize(size);
    bool ok = fread(data->data(), 1, size, file) == size && !ferror(file);
    if (fclose(file) != 0) ok = false;
    return ok;
}
bool FatFsStore::ReadFileChunks(const std::string& path, ChunkCallback callback, void* context) {
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) return false;
    uint8_t buffer[4096];
    bool ok = true;
    while (ok) {
        size_t n = fread(buffer, 1, sizeof(buffer), file);
        if (n) ok = callback(buffer, n, context);
        if (n < sizeof(buffer)) { if (ferror(file)) ok = false; break; }
        Progress();
    }
    if (fclose(file) != 0) ok = false;
    return ok;
}
bool FatFsStore::WriteFile(const std::string& path, const uint8_t* data, size_t length, bool append, size_t* written) {
    if (!written || (length && !data)) return false;
    *written = 0;
    FILE* file = fopen(path.c_str(), append ? "ab" : "wb");
    if (!file) return false;
    *written = length ? fwrite(data, 1, length, file) : 0;
    bool ok = *written == length && fflush(file) == 0;
    if (fclose(file) != 0) ok = false;
    return ok;
}
bool FatFsStore::SyncFile(const std::string& path) {
    FILE* file = fopen(path.c_str(), "rb+");
    if (!file) return false;
    bool ok = fflush(file) == 0 && fsync(fileno(file)) == 0;
    if (fclose(file) != 0) ok = false;
    return ok;
}
bool FatFsStore::SyncDirectory(const std::string& path) {
    // FatFS has no directory descriptors. f_sync/f_close update the directory
    // entry and call sync_fs; f_rename/f_unlink also call sync_fs before return.
    // SyncFile and Rename must therefore succeed before this adapter barrier.
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
bool FatFsStore::Rename(const std::string& from, const std::string& to) {
    // FatFS f_rename cannot replace an existing target. Store only replaces the
    // older snapshot slot (or a verified corrupt image), preserving its peer.
    struct stat st;
    if (stat(to.c_str(), &st) == 0 && (!S_ISREG(st.st_mode) || unlink(to.c_str()) != 0)) return false;
    return rename(from.c_str(), to.c_str()) == 0;
}
bool FatFsStore::Remove(const std::string& path) { return unlink(path.c_str()) == 0 || errno == ENOENT; }
bool FatFsStore::StatFile(const std::string& path, size_t* size) {
    struct stat st;
    if (!size || stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) return false;
    *size = static_cast<size_t>(st.st_size);
    return true;
}
bool FatFsStore::ListFiles(const std::string& path, std::vector<std::string>* paths) {
    DIR* dir = opendir(path.c_str());
    if (!dir || !paths) { if (dir) closedir(dir); return false; }
    paths->clear();
    bool ok = true;
    errno = 0;
    while (struct dirent* entry = readdir(dir)) {
        // Store subsequently checks its strict namespace; never recurse.
        if (entry->d_name[0] != '.') paths->push_back(path + "/" + entry->d_name);
        if (paths->size() > 4096) { ok = false; break; }
        errno = 0;
    }
    if (errno != 0) ok = false;
    if (closedir(dir) != 0) ok = false;
    return ok;
}
bool ValidateStoredBmp(FileSystem*, const std::string& path, const PhotoEntry& expected, void*) {
    bmp_validator_info_t info;
    return bmp_validator_validate_file(path.c_str(), &info) == BMP_VALIDATOR_OK &&
           info.width == expected.width && info.height == expected.height && info.total_size == expected.size;
}
}
