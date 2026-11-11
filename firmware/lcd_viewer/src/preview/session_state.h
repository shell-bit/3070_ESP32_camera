#pragma once
#include "session_clock.h"
#include <cstring>
namespace lcd {
// The caller serializes all access. Epochs fence late HTTP responses and old stream work.
class SessionState {
public:
    SessionClock clock;
    uint32_t generation=0;
    char id[97]={},retired[97]={};
    bool start(uint32_t now) {
        if(clock.active())return false;
        clock.start(now);++generation;return true;
    }
    void stop() {
        if(!clock.active())return;
        clock.stop();++generation;
        if(*id) { std::strcpy(retired,id);id[0]=0; }
    }
    bool adopt(uint32_t epoch,const char* created,uint32_t now) {
        if(epoch!=generation || !clock.active() || !created || !*created || std::strlen(created)>=sizeof id)return false;
        std::strcpy(id,created);clock.renewed(now);return true;
    }
    bool forget(uint32_t epoch) {
        if(epoch!=generation)return false;
        id[0]=0;++generation;return true;
    }
};
}
