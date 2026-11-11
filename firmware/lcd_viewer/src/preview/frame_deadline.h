#pragma once
#include <cstdint>
namespace lcd {
// Receive-task watchdog, separate from the UI's last-displayed-frame age.
class FrameDeadline {
public:
    bool expired(bool inFrame,uint64_t completedFrames,uint32_t now) {
        if(!inFrame) { tracking_=false;return false; }
        // A single feed may finish one JPEG and enter the next without ever
        // exposing a non-JPEG state to its caller. Completion fences the timer.
        if(!tracking_ || completedFrames!=completedFrames_) {
            tracking_=true;started_=now;completedFrames_=completedFrames;
        }
        return static_cast<uint32_t>(now-started_)>=2000;
    }
private:
    bool tracking_=false;
    uint32_t started_=0;
    uint64_t completedFrames_=0;
};
}
