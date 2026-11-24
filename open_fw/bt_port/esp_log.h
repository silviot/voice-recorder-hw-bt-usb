/* ESP-IDF logging mapped to openfw (BT_LOGS=0 compiles it all out, strings included). SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#include <stdarg.h>
typedef enum { ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_VERBOSE } esp_log_level_t;
void esp_log_write(esp_log_level_t level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
uint32_t esp_log_timestamp(void);
#define LOG_FORMAT(letter, format) #letter " (%" PRIu32 ") %s: " format "\n"
#include <inttypes.h>
#if defined(BT_LOGS) && BT_LOGS
void openfw_bt_log(char level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#define ESP_LOGE(t, f, ...) openfw_bt_log('E', t, f, ##__VA_ARGS__)
#define ESP_LOGW(t, f, ...) openfw_bt_log('W', t, f, ##__VA_ARGS__)
#define ESP_LOGI(t, f, ...) openfw_bt_log('I', t, f, ##__VA_ARGS__)
#define ESP_LOGD(t, f, ...) ((void)0)
#define ESP_LOGV(t, f, ...) ((void)0)
#else
#define ESP_LOGE(t, f, ...) ((void)(t))
#define ESP_LOGW(t, f, ...) ((void)(t))
#define ESP_LOGI(t, f, ...) ((void)(t))
#define ESP_LOGD(t, f, ...) ((void)(t))
#define ESP_LOGV(t, f, ...) ((void)(t))
#endif
#define ESP_EARLY_LOGE ESP_LOGE
#define ESP_EARLY_LOGW ESP_LOGW
#define ESP_EARLY_LOGI ESP_LOGI
#define ESP_LOG_BUFFER_HEX(t, b, l) ((void)0)
#define ESP_LOG_BUFFER_HEX_LEVEL(t, b, l, lv) ((void)0)
#define esp_rom_printf(...) ((void)0)
