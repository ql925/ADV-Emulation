#pragma once
// In-memory filesystem / Print substitutes for exercising the real FlashLog.
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <stdint.h>

class Print {
public:
    virtual ~Print() = default;
    virtual size_t write(uint8_t) = 0;
    virtual size_t write(const uint8_t* data, size_t size) {
        size_t n = 0;
        while (n < size && write(data[n])) ++n;
        return n;
    }
    size_t printf(const char* format, ...) {
        va_list args;
        va_start(args, format);
        va_list copy;
        va_copy(copy, args);
        const int size = vsnprintf(nullptr, 0, format, copy);
        va_end(copy);
        std::vector<char> buffer(size + 1);
        vsnprintf(buffer.data(), buffer.size(), format, args);
        va_end(args);
        return write(reinterpret_cast<const uint8_t*>(buffer.data()), size);
    }
    size_t println(const char* text) {
        return write(reinterpret_cast<const uint8_t*>(text), strlen(text)) +
               write(reinterpret_cast<const uint8_t*>("\r\n"), 2);
    }
};

#define FILE_WRITE "w"
#define FILE_READ "r"
struct FakeFS;
namespace fs {
class File {
public:
    File() = default;
    File(FakeFS* owner, std::string path, bool dir = false);
    explicit operator bool() const { return owner != nullptr; }
    bool isDirectory() const { return directory; }
    const char* name() const { return path.c_str(); }
    File openNextFile();
    size_t write(const uint8_t* data, size_t size);
    size_t read(uint8_t* data, size_t size);
    size_t size() const;
    bool available() const { return offset < size(); }
    void flush() {}
    bool getWriteError() const { return error; }
    void close() { owner = nullptr; }
private:
    FakeFS* owner{};
    std::string path;
    bool directory{}, error{};
    size_t index{}, offset{};
    std::vector<std::string> children;
};
}
struct FakeFS {
    std::map<std::string, std::string> files;
    std::vector<std::string> directories{"/log"};
    std::vector<std::string> removed;
    size_t capacity{1000000};
    bool remove_failure{}, write_failure{}, rename_failure{};
    bool mount_failure{}, format_failure{};
    unsigned format_attempts{};
    bool begin(bool format) {
        if (format) { ++format_attempts; return !format_failure; }
        return !mount_failure;
    }
    bool isDirectory(const std::string& path) const {
        return std::find(directories.begin(), directories.end(), path) != directories.end();
    }
    bool exists(const char* path) const { return files.count(path) || isDirectory(path); }
    bool mkdir(const char* path) { directories.push_back(path); return true; }
    size_t totalBytes() const { return capacity; }
    size_t usedBytes() const { size_t n = 0; for (const auto& file : files) n += file.second.size(); return n; }
    fs::File open(const char* path, const char* mode = FILE_READ) {
        if (!strcmp(mode, FILE_WRITE)) files[path].clear();
        return exists(path) ? fs::File(this, path, isDirectory(path)) : fs::File();
    }
    bool remove(const char* path) {
        if (remove_failure || !files.count(path)) return false;
        removed.push_back(path);
        files.erase(path);
        return true;
    }
    bool rename(const char* from, const char* to) {
        if (rename_failure || !files.count(from) || files.count(to)) return false;
        files[to] = files[from]; files.erase(from); return true;
    }
};
inline fs::File::File(FakeFS* owner, std::string path, bool dir)
    : owner(owner), path(path), directory(dir) {
    if (dir) {
        for (const auto& f : owner->files)
            if (f.first.find(path + "/") == 0) children.push_back(f.first);
        for (const auto& d : owner->directories)
            if (d.find(path + "/") == 0) children.push_back(d);
    }
}
inline fs::File fs::File::openNextFile() {
    return index < children.size() ? owner->open(children[index++].c_str()) : File();
}
inline size_t fs::File::size() const { return owner->files.at(path).size(); }
inline size_t fs::File::write(const uint8_t* data, size_t size) {
    const size_t free = owner->capacity > owner->usedBytes() ? owner->capacity - owner->usedBytes() : 0;
    const size_t n = owner->write_failure ? 0 : std::min(size, free);
    owner->files[path].append(reinterpret_cast<const char*>(data), n);
    error = n != size;
    return n;
}
inline size_t fs::File::read(uint8_t* data, size_t n) {
    n = std::min(n, size() - offset);
    memcpy(data, owner->files.at(path).data() + offset, n); offset += n; return n;
}
inline FakeFS LittleFS;
inline unsigned long millis() { return 1000; }
inline void delay(int) {}
