#pragma once
#include <cstdint>
namespace lcd {
enum class ScanState { Idle, Queued, Scanning, Ready, Failed, Cancelling };
struct NetworkEntry { char ssid[33]{}; int32_t rssi=0; uint8_t security=0; char suffix[9]{}; };
struct SetupNetworkSnapshot {
    uint32_t operationId=0; ScanState scanState=ScanState::Idle; uint8_t count=0;
    NetworkEntry networks[16]; bool connected=false; char ssid[33]{},ip[16]{};
    int32_t rssi=0; char error[64]{};
};
}
