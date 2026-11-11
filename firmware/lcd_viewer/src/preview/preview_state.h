#pragma once
#include <cstdint>

namespace lcd {
struct PreviewSnapshot {
    bool active=false,configured=false,wifi=false,stale=true;
    uint32_t generation=0,remainingMs=0,frameAgeMs=0;
    uint64_t sequence=0;
    char state[40]="idle";
    char error[80]={};
};
}
