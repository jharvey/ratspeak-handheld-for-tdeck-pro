#pragma once
// Partition table shim: one 6 MB "spiffs" data partition (LittleFS lives in a host
// directory, so raw partition reads see erased flash).
#include <cstddef>
#include <cstdint>
#include "esp_system.h"
typedef enum { ESP_PARTITION_TYPE_APP = 0x00, ESP_PARTITION_TYPE_DATA = 0x01,
               ESP_PARTITION_TYPE_ANY = 0xff } esp_partition_type_t;
typedef enum {
    ESP_PARTITION_SUBTYPE_APP_FACTORY = 0x00, ESP_PARTITION_SUBTYPE_APP_OTA_0 = 0x10,
    ESP_PARTITION_SUBTYPE_APP_OTA_1 = 0x11, ESP_PARTITION_SUBTYPE_APP_OTA_2 = 0x12,
    ESP_PARTITION_SUBTYPE_DATA_NVS = 0x02, ESP_PARTITION_SUBTYPE_DATA_SPIFFS = 0x82,
    ESP_PARTITION_SUBTYPE_ANY = 0xff
} esp_partition_subtype_t;
typedef struct {
    void* flash_chip;
    esp_partition_type_t type;
    esp_partition_subtype_t subtype;
    uint32_t address;
    uint32_t size;
    uint32_t erase_size;
    char label[17];
    bool encrypted;
} esp_partition_t;
#ifdef __cplusplus
extern "C" {
#endif
const esp_partition_t* esp_partition_find_first(esp_partition_type_t, esp_partition_subtype_t,
                                                const char* label);
esp_err_t esp_partition_read(const esp_partition_t*, size_t off, void* dst, size_t n);
esp_err_t esp_partition_write(const esp_partition_t*, size_t off, const void* src, size_t n);
esp_err_t esp_partition_erase_range(const esp_partition_t*, size_t off, size_t n);
#ifdef __cplusplus
}
#endif
