#pragma once
void worker_test_log(const char *tag, const char *format, ...);
#define ESP_LOGI(...) worker_test_log(__VA_ARGS__)
