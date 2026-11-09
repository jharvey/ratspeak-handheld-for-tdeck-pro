#pragma once
#include <cstddef>
#include <cstdint>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_TIMEOUT 0x107
typedef enum { ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW, ESP_RST_PANIC,
               ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_WDT, ESP_RST_DEEPSLEEP,
               ESP_RST_BROWNOUT, ESP_RST_SDIO } esp_reset_reason_t;
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t esp_efuse_mac_get_default(uint8_t* mac);
esp_err_t esp_read_mac(uint8_t* mac, int type);
void esp_fill_random(void* buf, size_t len);
uint32_t esp_random(void);
esp_reset_reason_t esp_reset_reason(void);
void esp_restart(void);
uint32_t esp_get_free_heap_size(void);
int64_t esp_timer_get_time(void);
const char* esp_err_to_name(esp_err_t);
#ifdef __cplusplus
}
#endif
