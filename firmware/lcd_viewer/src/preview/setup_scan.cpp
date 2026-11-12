#include "setup_scan.h"
#include <cstring>
#include <cstdio>
namespace lcd {
bool SetupScan::busy() const {
    return state_.scanState==ScanState::Queued || state_.scanState==ScanState::Scanning || state_.scanState==ScanState::Cancelling;
}
bool SetupScan::request(uint32_t id,uint32_t now) {
    if(!id || busy())return false;
    state_=SetupNetworkSnapshot{};state_.operationId=id;state_.scanState=ScanState::Queued;
    started_=now;owned_=cancelled_=failed_=false;return true;
}
void SetupScan::cancel(uint32_t id) {
    if(!id || id!=state_.operationId)return;
    state_.operationId=0;state_.count=0;cancelled_=true;
    state_.scanState=busy()?ScanState::Cancelling:ScanState::Idle;
}
void SetupScan::fail(const char* error) {
    failed_=true;std::snprintf(state_.error,sizeof state_.error,"%s",error);state_.scanState=ScanState::Cancelling;
}
void SetupScan::finish() {
    if(owned_ && !radio_.drain())return;
    if(owned_)radio_.release();
    owned_=false;state_.scanState=cancelled_?ScanState::Idle:failed_?ScanState::Failed:ScanState::Ready;
}
void SetupScan::insert(const NetworkEntry& value) {
    if(!value.ssid[0])return;
    NetworkEntry item=value;item.ssid[32]=0;item.suffix[8]=0;
    for(unsigned i=0;i<state_.count;++i) {
        if(!std::strcmp(item.ssid,state_.networks[i].ssid) && item.security==state_.networks[i].security) {
            if(item.rssi<=state_.networks[i].rssi)return;
            for(unsigned j=i+1;j<state_.count;++j)state_.networks[j-1]=state_.networks[j];
            --state_.count;break;
        }
    }
    unsigned at=0;
    while(at<state_.count && state_.networks[at].rssi>=item.rssi)++at;
    if(at>=16)return;
    if(state_.count<16)++state_.count;
    for(unsigned i=state_.count-1;i>at;--i)state_.networks[i]=state_.networks[i-1];
    state_.networks[at]=item;
}
void SetupScan::tick(uint32_t now,bool priorWorkIdle) {
    if(!busy())return;
    if(state_.scanState==ScanState::Cancelling) { finish();return; }
    if(now-started_>=12000U) { fail("Wi-Fi scan timed out");finish();return; }
    if(state_.scanState==ScanState::Queued) {
        if(!priorWorkIdle)return;
        owned_=true;
        if(!radio_.acquire() || !radio_.start()) { fail("Wi-Fi scan could not start");finish();return; }
        state_.scanState=ScanState::Scanning;return;
    }
    if(!radio_.done())return;
    const int n=radio_.count();
    if(n<0)fail("Wi-Fi scan failed");
    else for(int i=0;i<n;++i) { NetworkEntry item;if(radio_.entry(i,item))insert(item); }
    finish();
}
}
