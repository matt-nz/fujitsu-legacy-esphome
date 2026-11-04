#pragma once
void fake_log(char level, const char *fmt, ...);
#define ESP_LOGD(tag, ...) fake_log('D', __VA_ARGS__)
#define ESP_LOGI(tag, ...) fake_log('I', __VA_ARGS__)
#define ESP_LOGW(tag, ...) fake_log('W', __VA_ARGS__)
#define ESP_LOGE(tag, ...) fake_log('E', __VA_ARGS__)
