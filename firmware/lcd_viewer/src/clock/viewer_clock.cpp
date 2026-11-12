#include "viewer_clock.h"
#include <Arduino.h>
#include <esp_sntp.h>
#include <atomic>
#include <ctime>

namespace lcd {
namespace {
std::atomic<bool> synchronized{false};
bool started = false;
bool online = false;

void onTimeSync(struct timeval* value) {
    // This callback runs on a network task; it never touches LVGL.
    synchronized.store(value && value->tv_sec >= 1704067200LL && value->tv_sec < 4102444800LL);
}
}

void viewerClockBegin() {
    synchronized.store(false);
    started = online = false;
    sntp_set_time_sync_notification_cb(onTimeSync);
}

void viewerClockLoop(bool wifi) {
    online = wifi;
    if (wifi && !started) {
        // Starts asynchronous SNTP; never wait for getLocalTime() on the UI task.
        configTzTime("CST-8", "pool.ntp.org", "time.cloudflare.com");
        started = true;
    }
}

ClockSnapshot viewerClockSnapshot() {
    return makeClockSnapshot(static_cast<int64_t>(std::time(nullptr)), synchronized.load(), online);
}
}
