#define ARDUINO 1
#include "../main/FlashLog.h"
#include <stdexcept>
#include <iostream>

void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
std::string log_path(unsigned id) {
    char path[40]; snprintf(path, sizeof(path), "/log/nfc-%08u.txt", id); return path;
}
auto record = [](auto& out) { for (unsigned i = 0; i < 100; ++i) out.println("test payload"); };

int main() {
    try {
        // A failed mount must explain itself and never erase existing contents.
        LittleFS = FakeFS{};
        partition_present = false;
        FlashLog missing;
        require(!missing.begin() && !missing.ready(), "missing partition");
        require(std::string(missing.display_status()) == "ERROR: no partition", "missing partition display");
        partition_present = true;
        LittleFS.mount_failure = true;
        partition_erased = false;
        LittleFS.files["/keep.bin"] = "existing contents";
        FlashLog occupied;
        require(!occupied.begin(), "reject unmountable existing partition");
        require(LittleFS.format_attempts == 0 && LittleFS.files.at("/keep.bin") == "existing contents",
                "mount error must not format existing data");
        require(std::string(occupied.display_status()) == "ERROR: FS mount (serial)", "mount error display");
        partition_read_error = true;
        FlashLog unreadable;
        require(!unreadable.begin() && LittleFS.format_attempts == 0, "read error must not format");
        require(std::string(unreadable.display_status()) == "ERROR: Flash read", "read error display");
        partition_read_error = false;
        partition_erased = true;
        LittleFS = FakeFS{};
        LittleFS.mount_failure = true;
        FlashLog blank;
        require(blank.begin() && LittleFS.format_attempts == 1, "provision fully erased partition");
        require(std::string(blank.display_status()) == "Flash ready", "ready display");

        // Full filesystem: remove oldest logs until this specific record fits.
        LittleFS = FakeFS{};
        LittleFS.capacity = 30000;
        LittleFS.files["/other.bin"] = std::string(10000, 'x');
        for (unsigned i = 1; i <= 8; ++i) LittleFS.files[log_path(i)] = std::string(2000, 'x');
        LittleFS.directories.push_back("/log/nfc-00000099.txt");
        FlashLog full;
        require(full.begin(), "begin full");
        require(full.save("test", record), "save should reclaim old logs");
        require(LittleFS.removed.size() == 3, "only the required oldest logs should be reclaimed");
        for (unsigned i = 0; i < 3; ++i) require(LittleFS.removed[i] == log_path(i + 1), "oldest-first");
        require(LittleFS.files.count(log_path(8)) && LittleFS.files.count("/other.bin"), "preserve latest and unrelated");
        require(std::string(full.last_path()) == log_path(9), "directory must not affect sequence");
        require(LittleFS.files.at(log_path(9)).find("==== END NFC diagnostic ====") != std::string::npos, "complete marker");

        LittleFS = FakeFS{};
        LittleFS.capacity = 12000;
        LittleFS.files[log_path(1)] = std::string(5000, 'x');
        LittleFS.files["/other.bin"] = std::string(2000, 'x');
        FlashLog small;
        require(small.begin() && !small.save("test", record), "reject when newest must be retained");
        require(LittleFS.removed.empty() && LittleFS.files.count(log_path(1)), "never delete last completed log");

        // An interrupted temporary record is reclaimable without sacrificing a complete log.
        LittleFS = FakeFS{};
        LittleFS.capacity = 16000;
        LittleFS.files[log_path(1)] = std::string(2000, 'x');
        LittleFS.files["/log/nfc-pending.tmp"] = std::string(6000, 'x');
        FlashLog interrupted;
        require(interrupted.begin() && interrupted.save("test", record), "reclaim temp");
        require(LittleFS.removed.size() == 1 && LittleFS.removed[0] == "/log/nfc-pending.tmp", "only temp removed");

        for (unsigned error = 0; error < 3; ++error) {
            LittleFS = FakeFS{};
            LittleFS.files[log_path(1)] = "previous complete";
            FlashLog failed;
            require(failed.begin(), "begin failure case");
            if (error == 0) LittleFS.write_failure = true;
            if (error == 1) LittleFS.rename_failure = true;
            if (error == 2) {
                LittleFS.capacity = 12000;
                LittleFS.files[log_path(2)] = std::string(5000, 'x');
                LittleFS.remove_failure = true;
            }
            require(!failed.save("test", record), "fault must not report success");
            require(std::string(failed.last_path()) == log_path(1), "failed save must not change last path");
            require(LittleFS.files.count(log_path(1)), "failed save preserves previous complete log");
            LittleFS.write_failure = LittleFS.rename_failure = LittleFS.remove_failure = false;
            LittleFS.capacity = 1000000;
            if (error == 2) LittleFS.files.erase(log_path(2));
            require(failed.save("test", record), "retry after fault");
            require(std::string(failed.last_path()) == log_path(2), "failed attempt must not consume sequence");
        }

        LittleFS = FakeFS{};
        for (unsigned i = 1; i <= 8; ++i) LittleFS.files[log_path(i)] = "old";
        FlashLog rotation;
        require(rotation.begin() && rotation.save("test", record), "ordinary rotation");
        require(LittleFS.files.size() == 8 && !LittleFS.files.count(log_path(1)), "retain eight logs");

        LittleFS = FakeFS{};
        for (unsigned i = 1; i <= 9; ++i) LittleFS.files[log_path(i)] = "old";
        FlashLog warning;
        require(warning.begin(), "begin rotation warning");
        LittleFS.remove_failure = true;
        require(warning.save("test", record), "rotation failure still saved successfully");
        require(std::string(warning.display_status()) == "saved; rotation warning", "do not display save as error");
        require(LittleFS.files.count(log_path(10)), "saved file survives rotation warning");

        LittleFS = FakeFS{};
        LittleFS.capacity = 9000;
        LittleFS.files[log_path(1)] = "old";
        LittleFS.files[log_path(2)] = "latest";
        FlashLog oversized;
        require(oversized.begin() && !oversized.save("test", record), "oversized log rejected");
        require(LittleFS.removed.empty(), "oversized record must not delete history");
        std::cout << "FlashLog host tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
