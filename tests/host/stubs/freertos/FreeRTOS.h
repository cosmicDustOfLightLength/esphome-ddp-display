// Minimal stub for host tests.
#pragma once
#include <thread>
#include <chrono>
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
inline int xTaskCreate(void(*f)(void*),const char*,int,void*a,int,void*){ std::thread(f,a).detach(); return pdPASS; }
inline void vTaskDelay(int ms){ std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
