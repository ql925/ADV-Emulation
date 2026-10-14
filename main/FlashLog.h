#pragma once

#if defined(ARDUINO)
#include <LittleFS.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

// Flash writes are only called with emulation stopped and no external RF field.
// Never format an existing filesystem merely because mounting failed.
class FlashLog {
public:
    bool begin()
    {
        _boot = esp_random();
        const auto* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                    ESP_PARTITION_SUBTYPE_ANY, "spiffs");
        if (!part) return fail("no spiffs partition; select Default partition scheme");
        if (!LittleFS.begin(false)) {
            uint8_t buffer[1024];
            for (size_t offset = 0; offset < part->size; offset += sizeof(buffer)) {
                const size_t size = std::min<size_t>(sizeof(buffer), part->size - offset);
                if (esp_partition_read(part, offset, buffer, size) != ESP_OK)
                    return fail("partition read failed; not formatted");
                for (size_t i = 0; i < size; ++i)
                    if (buffer[i] != 0xFF) return fail("unmountable non-empty partition; not formatted");
                delay(0);
            }
            // Entire partition verified erased: provision the new log filesystem.
            if (!LittleFS.begin(true)) return fail("cannot initialize blank LittleFS");
        }
        if (!LittleFS.exists("/log") && !LittleFS.mkdir("/log")) return fail("cannot create /log");
        auto dir = LittleFS.open("/log");
        if (!dir || !dir.isDirectory()) return fail("/log is not a directory");
        for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
            if (file.isDirectory()) continue;
            uint32_t id = file_id(file.name());
            if (id >= _next) {
                _next = id + 1;
                snprintf(_last, sizeof(_last), "/log/nfc-%08lu.txt", static_cast<unsigned long>(id));
            }
        }
        if (_next > 99999999) return fail("log sequence exhausted");
        _ready = true;
        _status = "ready";
        return true;
    }

    bool ready() const { return _ready; }
    const char* status() const { return _status; }
    // Compact enough for the Cardputer status line. Detailed reason remains in logs.
    const char* display_status() const
    {
        if (!strcmp(_status, "saved")) return "/log (Flash)";
        if (!strcmp(_status, "saved; rotation failed")) return "saved; rotation warning";
        if (!strcmp(_status, "ready")) return "Flash ready";
        if (!strcmp(_status, "not initialized")) return "not initialized";
        if (!strncmp(_status, "no spiffs partition", 19)) return "ERROR: no partition";
        if (!strcmp(_status, "unmountable non-empty partition; not formatted")) return "ERROR: FS mount (serial)";
        if (!strcmp(_status, "partition read failed; not formatted")) return "ERROR: Flash read";
        if (!strcmp(_status, "cannot initialize blank LittleFS")) return "ERROR: FS init";
        if (!strcmp(_status, "cannot create /log") || !strcmp(_status, "/log is not a directory"))
            return "ERROR: /log directory";
        if (!strcmp(_status, "log sequence exhausted")) return "ERROR: log sequence";
        return "ERROR: save (serial)";
    }
    const char* last_path() const { return _last; }

    // A checked, buffered Print sink avoids hundreds of tiny filesystem writes.
    class Sink : public Print {
    public:
        explicit Sink(fs::File& file) : _file(file) {}
        using Print::write;
        size_t write(uint8_t b) override { return write(&b, 1); }
        size_t write(const uint8_t* data, size_t size) override
        {
            if (!_ok) return 0;
            for (size_t i = 0; i < size; ++i) {
                _buffer[_used++] = data[i];
                if (_used == sizeof(_buffer) && !flush_buffer()) return i;
            }
            return size;
        }
        bool flush_buffer()
        {
            if (_ok && _used) _ok = _file.write(_buffer, _used) == _used;
            _used = 0;
            return _ok;
        }
    private:
        fs::File& _file;
        uint8_t _buffer[512]{};
        size_t _used{};
        bool _ok{true};
    };

    template <class Writer>
    bool save(const char* build, Writer writer)
    {
        if (!_ready) return false;
        if (_next > 99999999) return io_fail("log sequence exhausted");
        // Writer must be side-effect free: count the serialized record before I/O.
        // Freeze uptime so both passes produce exactly the same header.
        const unsigned long uptime = millis();
        CountSink counter;
        write_record(counter, build, uptime, writer);
        if (!reserve_space(counter.size())) return false;
        // A failed/power-interrupted write stays .tmp and is never presented as complete.
        auto file = LittleFS.open("/log/nfc-pending.tmp", FILE_WRITE);
        if (!file) return io_fail("open failed (filesystem full?)");
        Sink out(file);
        write_record(out, build, uptime, writer);
        const bool ok = out.flush_buffer();
        file.flush();
        const bool write_error = file.getWriteError();
        file.close();
        if (!ok || write_error) return io_fail("write failed; RAM trace retained");
        char path[40];
        snprintf(path, sizeof(path), "/log/nfc-%08lu.txt", static_cast<unsigned long>(_next));
        if (!LittleFS.rename("/log/nfc-pending.tmp", path)) return io_fail("rename failed; RAM trace retained");
        ++_next;
        snprintf(_last, sizeof(_last), "%s", path);
        _status = "saved";
        // Keep at most eight completed files. Remove only our exact filename pattern.
        if (!prune()) _status = "saved; rotation failed";
        return true;
    }

    void list(Print& out)
    {
        out.printf("FLASH_LOG=%s TOTAL=%u USED=%u\n", _status,
                   _ready ? static_cast<unsigned>(LittleFS.totalBytes()) : 0,
                   _ready ? static_cast<unsigned>(LittleFS.usedBytes()) : 0);
        if (!_ready) return;
        auto dir = LittleFS.open("/log");
        for (auto file = dir.openNextFile(); file; file = dir.openNextFile())
            if (file_id(file.name())) out.printf("%s %u bytes\n", file.name(), static_cast<unsigned>(file.size()));
    }

    void cat(const char* name, Print& out)
    {
        if (!_ready) { out.printf("FLASH_LOG=%s\n", _status); return; }
        if (!name || !*name) name = _last;
        const uint32_t id = file_id(name);
        if (!id) { out.println("Use: logs or logcat nfc-00000001.txt"); return; }
        char path[40];
        snprintf(path, sizeof(path), "/log/nfc-%08lu.txt", static_cast<unsigned long>(id));
        auto file = LittleFS.open(path, FILE_READ);
        if (!file) { out.println("Log not found"); return; }
        out.printf("==== FILE %s ====\n", path);
        uint8_t buffer[256];
        while (file.available()) {
            const size_t count = file.read(buffer, sizeof(buffer));
            if (!count) break;
            out.write(buffer, count);
            delay(0);
        }
    }

private:
    class CountSink : public Print {
    public:
        size_t write(uint8_t) override { ++_size; return 1; }
        size_t write(const uint8_t*, size_t size) override { _size += size; return size; }
        size_t size() const { return _size; }
    private:
        size_t _size{};
    };
    template <class Output, class Writer>
    void write_record(Output& out, const char* build, unsigned long uptime, Writer& writer)
    {
        out.printf("Build=%s BOOT=%08lX FILE=%lu UPTIME_MS=%lu\n", build,
                   static_cast<unsigned long>(_boot), static_cast<unsigned long>(_next), uptime);
        writer(out);
        out.println("==== END NFC diagnostic ====");
    }
    bool oldest_log(uint32_t& oldest, unsigned& count)
    {
        oldest = UINT32_MAX;
        count = 0;
        auto dir = LittleFS.open("/log");
        if (!dir || !dir.isDirectory()) return false;
        for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
            if (file.isDirectory()) continue;
            const uint32_t id = file_id(file.name());
            if (id) { ++count; if (id < oldest) oldest = id; }
        }
        return true;
    }
    bool remove_log(uint32_t id)
    {
        char path[40];
        snprintf(path, sizeof(path), "/log/nfc-%08lu.txt", static_cast<unsigned long>(id));
        return LittleFS.remove(path);
    }
    bool reserve_space(size_t record_bytes)
    {
        // Leave two 4-KiB blocks for filesystem metadata / copy-on-write. This is
        // headroom, not a guarantee: all write/flush/rename checks still apply.
        constexpr size_t HEADROOM = 8192;
        const size_t total = LittleFS.totalBytes();
        if (total <= HEADROOM || record_bytes > total - HEADROOM)
            return io_fail("log exceeds filesystem capacity; RAM trace retained");
        const size_t required = record_bytes + HEADROOM;
        if (LittleFS.exists("/log/nfc-pending.tmp") && !LittleFS.remove("/log/nfc-pending.tmp"))
            return io_fail("cannot remove incomplete log; RAM trace retained");
        for (;;) {
            const size_t used = LittleFS.usedBytes();
            if (used <= total && total - used >= required) return true;
            uint32_t oldest;
            unsigned count;
            if (!oldest_log(oldest, count)) return io_fail("cannot scan logs; RAM trace retained");
            // Keep the newest completed diagnostic even if saving becomes impossible.
            if (count <= 1) return io_fail("insufficient space; newest log and RAM trace retained");
            if (!remove_log(oldest)) return io_fail("cannot free log space; RAM trace retained");
        }
    }
    static uint32_t file_id(const char* path)
    {
        const char* name = strrchr(path, '/');
        name = name ? name + 1 : path;
        if (strlen(name) != 16 || strncmp(name, "nfc-", 4) || strcmp(name + 12, ".txt")) return 0;
        uint32_t value = 0;
        for (unsigned i = 4; i < 12; ++i) {
            if (name[i] < '0' || name[i] > '9') return 0;
            value = 10 * value + name[i] - '0';
        }
        return value;
    }
    bool prune()
    {
        for (;;) {
            uint32_t oldest;
            unsigned count;
            if (!oldest_log(oldest, count)) return false;
            if (count <= 8) return true;
            if (!remove_log(oldest)) return false;
        }
    }
    bool fail(const char* message) { _status = message; _ready = false; return false; }
    bool io_fail(const char* message) { _status = message; return false; }
    bool _ready{};
    uint32_t _boot{}, _next{1};
    const char* _status{"not initialized"};
    char _last[40]{};
};
#endif
