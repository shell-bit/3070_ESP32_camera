#pragma once
#include <cstdint>
namespace lcd {
class SessionClock {
public:
    void start(uint32_t now) { active_=true; started_=renewed_=now; haveFrame_=false; }
    void stop() { active_=false;haveFrame_=false; }
    void extend(uint32_t now) { if(active_) started_=now; }
    void renewed(uint32_t now) { renewed_=now; }
    void frame(uint32_t now) { lastFrame_=now;haveFrame_=true; }
    bool active() const { return active_; }
    bool expired(uint32_t now) const { return active_ && static_cast<uint32_t>(now-started_)>=60000; }
    bool renewDue(uint32_t now) const { return active_ && static_cast<uint32_t>(now-renewed_)>=10000; }
    bool stale(uint32_t now) const { return !haveFrame_ || static_cast<uint32_t>(now-lastFrame_)>=1000; }
    uint32_t remaining(uint32_t now) const { const uint32_t elapsed=now-started_;return active_ && elapsed<60000?60000-elapsed:0; }
private:
    bool active_=false,haveFrame_=false;
    uint32_t started_=0,renewed_=0,lastFrame_=0;
};
}
