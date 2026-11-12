#pragma once
#include "setup_network_state.h"
namespace lcd {
// The adapter must drain both SDK and Arduino callbacks before returning true.
struct ScanRadio {
    virtual ~ScanRadio()=default;
    virtual bool acquire()=0;
    virtual bool start()=0;
    virtual bool done()=0;
    virtual int count()=0;
    virtual bool entry(int index,NetworkEntry& out)=0;
    virtual bool drain()=0;
    virtual void release()=0;
};
class SetupScan {
public:
    explicit SetupScan(ScanRadio& radio):radio_(radio) {}
    bool request(uint32_t id,uint32_t now);
    void cancel(uint32_t id);
    void tick(uint32_t now,bool priorWorkIdle);
    bool busy() const;
    const SetupNetworkSnapshot& snapshot() const { return state_; }
private:
    ScanRadio& radio_; SetupNetworkSnapshot state_;
    uint32_t started_=0; bool owned_=false, cancelled_=false, failed_=false;
    void finish();
    void fail(const char* error);
    void insert(const NetworkEntry& entry);
};
}
