#include "viewer_config.h"
#include <Preferences.h>
#include <cJSON.h>
#include <cstring>
#include <cctype>
namespace lcd {
namespace {
bool field(cJSON* root,const char* name,char* destination,size_t capacity) {
    auto* item=cJSON_GetObjectItemCaseSensitive(root,name);
    if(!cJSON_IsString(item) || !item->valuestring || std::strlen(item->valuestring)>=capacity)return false;
    std::strcpy(destination,item->valuestring);return true;
}
bool valid(const ViewerConfig& c) {
    if(!c.ready() || !c.controlPort || !c.streamPort)return false;
    for(const unsigned char* p=reinterpret_cast<const unsigned char*>(c.host);*p;++p)if(!std::isalnum(*p) && *p!='.' && *p!='-')return false;
    for(const unsigned char* p=reinterpret_cast<const unsigned char*>(c.token);*p;++p)if(!std::isalnum(*p) && !std::strchr("-._~+/=",*p))return false;
    return true;
}
}
bool loadConfig(ViewerConfig& config) {
    Preferences prefs;if(!prefs.begin("lcd-preview",true))return false;
    const bool found=prefs.getBytesLength("config")==sizeof(config);
    if(found)prefs.getBytes("config",&config,sizeof(config));prefs.end();
    config.ssid[32]=config.password[64]=config.host[127]=config.token[128]=0;
    return found && valid(config);
}
bool pollConfigSerial() {
    static char input[512];static size_t used=0;static bool overflow=false;
    while(Serial.available()) {
        char c=char(Serial.read());if(c=='\r')continue;
        if(c!='\n') { if(used<sizeof(input)-1)input[used++]=c;else overflow=true;continue; }
        input[used]=0;used=0;
        if(overflow) { overflow=false;Serial.println("Config rejected: too long");continue; }
        cJSON* json=cJSON_Parse(input);ViewerConfig config;
        bool ok=json && field(json,"wifi_ssid",config.ssid,sizeof config.ssid) && field(json,"wifi_password",config.password,sizeof config.password) && field(json,"camera_host",config.host,sizeof config.host) && field(json,"token",config.token,sizeof config.token);
        for(const char* key:{"control_port","stream_port"}) {
            auto* value=cJSON_GetObjectItemCaseSensitive(json,key);
            if(value) { if(!cJSON_IsNumber(value) || value->valuedouble<1 || value->valuedouble>65535 || value->valuedouble!=value->valueint)ok=false;
                else if(std::strcmp(key,"control_port")==0)config.controlPort=value->valueint;else config.streamPort=value->valueint; }
        }
        cJSON_Delete(json);std::memset(input,0,sizeof input);
        if(!ok || !valid(config)) { Serial.println("Config rejected: invalid fields");continue; }
        Preferences prefs;if(!prefs.begin("lcd-preview",false)) { Serial.println("Config storage unavailable");continue; }
        ok=prefs.putBytes("config",&config,sizeof config)==sizeof config;prefs.end();
        std::memset(&config,0,sizeof config);
        Serial.println(ok?"Config saved; restarting":"Config save failed");
        if(ok) { delay(100);ESP.restart();return true; }
    }
    return false;
}
}
