#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>

class FakeSerial {
public:
    std::deque<unsigned char> input;
    std::string output;

    int available() const { return static_cast<int>(input.size()); }
    int read() {
        if (input.empty()) return -1;
        const int value = input.front();
        input.pop_front();
        return value;
    }
    void println(const char* value) { output += std::string(value) + '\n'; }
    void feed(const std::string& value) {
        input.insert(input.end(), value.begin(), value.end());
    }
};

struct FakeEsp {
    unsigned restarts = 0;
    void restart() { ++restarts; }
};

inline FakeSerial Serial;
inline FakeEsp ESP;
inline unsigned long delayedMilliseconds = 0;
inline void delay(unsigned long milliseconds) { delayedMilliseconds += milliseconds; }
