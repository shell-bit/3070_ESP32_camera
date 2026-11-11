#pragma once
#include <Arduino.h>
namespace lcd {
struct ViewerConfig {
    char ssid[33]={}; char password[65]={}; char host[128]={}; char token[129]={};
    uint16_t controlPort=80,streamPort=81;
    bool ready() const { return ssid[0] && host[0] && token[0]; }
};
bool loadConfig(ViewerConfig& config);
// JSON provisioning over local USB serial while idle. Never echoes secrets.
bool pollConfigSerial();
}
