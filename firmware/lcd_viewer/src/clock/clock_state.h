#pragma once
#include <cstdint>
#include <ctime>

namespace lcd {
struct ClockSnapshot {
    bool valid = false;
    bool online = false;
    char time[9] = "--:--:--";
    char date[11] = "---- -- --";
};

// The display uses UTC+8 explicitly; firmware/session timers remain monotonic.
inline ClockSnapshot makeClockSnapshot(int64_t epoch, bool synchronized, bool online) {
    ClockSnapshot result;
    result.online = online;
    // Reject the boot epoch and malformed/out-of-range network time.
    if (!synchronized || epoch < 1704067200LL || epoch >= 4102444800LL) return result;
    const std::time_t localEpoch = static_cast<std::time_t>(epoch + 8 * 60 * 60);
    std::tm local{};
#ifdef _WIN32
    if (gmtime_s(&local, &localEpoch) != 0) return result;
#else
    if (!gmtime_r(&localEpoch, &local)) return result;
#endif
    if (std::strftime(result.time, sizeof result.time, "%H:%M:%S", &local) != 8 ||
        std::strftime(result.date, sizeof result.date, "%Y-%m-%d", &local) != 10)
        return ClockSnapshot{};
    result.valid = true;
    return result;
}
}
