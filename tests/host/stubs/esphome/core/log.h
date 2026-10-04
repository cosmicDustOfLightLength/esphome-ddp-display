// Minimal stub for host tests.
#pragma once
#include <cstdio>
#define ESP_LOGE(t,f,...) printf("[E][%s] " f "\n",t,##__VA_ARGS__)
#define ESP_LOGW ESP_LOGE
#define ESP_LOGI ESP_LOGE
#define ESP_LOGCONFIG ESP_LOGE
