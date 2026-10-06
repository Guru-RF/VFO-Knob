#ifndef SHIM_ESP_LOG_H
#define SHIM_ESP_LOG_H
void shim_log(char lv, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#define ESP_LOGE(tag, ...) shim_log('E', tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) shim_log('W', tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) shim_log('I', tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) shim_log('D', tag, __VA_ARGS__)
#endif
