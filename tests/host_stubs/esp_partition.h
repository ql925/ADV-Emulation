#pragma once
#include <cstddef>
#include <cstring>
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_SUBTYPE_ANY 0
#define ESP_OK 0
struct esp_partition_t { size_t size; };
inline bool partition_present = true;
inline bool partition_erased = true;
inline bool partition_read_error = false;
inline const esp_partition_t* esp_partition_find_first(int, int, const char*) {
    static esp_partition_t part{1000000}; return partition_present ? &part : nullptr;
}
inline int esp_partition_read(const esp_partition_t*, size_t, void* data, size_t size) {
    if (partition_read_error) return -1;
    memset(data, partition_erased ? 0xFF : 0x00, size); return ESP_OK;
}
