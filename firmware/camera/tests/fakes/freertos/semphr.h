#pragma once
#include <mutex>
using SemaphoreHandle_t=std::mutex*;
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
inline SemaphoreHandle_t xSemaphoreCreateMutex(){static std::mutex mutex;return &mutex;}
extern bool fakeBlockLock;
inline int xSemaphoreTake(SemaphoreHandle_t m,unsigned long){return !fakeBlockLock&&m->try_lock()?1:0;}
inline void xSemaphoreGive(SemaphoreHandle_t m){m->unlock();}
