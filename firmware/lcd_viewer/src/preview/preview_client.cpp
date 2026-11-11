#include "preview_client.h"
#include "mjpeg_parser.h"
#include "frame_deadline.h"
#include "session_clock.h"
#include "session_state.h"
#include "../board/jpeg_display.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <cJSON.h>
#include <cstring>
#include <cctype>

namespace lcd {
namespace {
ViewerConfig settings;
SemaphoreHandle_t stateMutex=nullptr,frameMutex=nullptr;
SessionState lifecycle;
SessionClock& watch=lifecycle.clock;
auto& session=lifecycle.id;
auto& retiredSession=lifecycle.retired;
auto& generation=lifecycle.generation;
char requestId[49]={},cameraState[40]="idle",lastError[80]={};
uint32_t lastDisplayAt=0;uint64_t lastSequence=0;
uint8_t* incoming=nullptr;uint8_t* pending=nullptr;
size_t pendingLength=0;uint64_t pendingSequence=0;
uint32_t pendingGeneration=0,pendingAt=0;
bool configured=false;
struct Lock {
    SemaphoreHandle_t mutex;
    explicit Lock(SemaphoreHandle_t m):mutex(m) { xSemaphoreTake(m,portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(mutex); }
};
struct ControlView { bool active;uint32_t gen;bool renew;char id[97];char request[49]; };
void newRequest() { snprintf(requestId,sizeof requestId,"lcd-%08lx%08lx-%08lx",static_cast<unsigned long>(esp_random()),static_cast<unsigned long>(esp_random()),static_cast<unsigned long>(generation)); }
ControlView view() {
    Lock lock(stateMutex);ControlView v={};v.active=watch.active();v.gen=generation;v.renew=watch.renewDue(millis());
    strlcpy(v.id,session,sizeof v.id);strlcpy(v.request,requestId,sizeof v.request);return v;
}
void setError(uint32_t gen,const char* message) { Lock lock(stateMutex);if(gen==generation)strlcpy(lastError,message,sizeof lastError); }
bool safeId(const char* id) {
    if(!id || !*id || strlen(id)>96)return false;
    for(const unsigned char* p=reinterpret_cast<const unsigned char*>(id);*p;++p)if(!std::isalnum(*p) && *p!='-' && *p!='_')return false;
    return true;
}
class ResponseBuffer final:public Stream {
public:
    char bytes[4097]={};size_t used=0;
    size_t write(uint8_t b) override { return write(&b,1); }
    size_t write(const uint8_t* data,size_t n) override { if(n>4096-used)return 0;memcpy(bytes+used,data,n);used+=n;bytes[used]=0;return n; }
    int available() override { return 0; } int read() override { return -1; } int peek() override { return -1; } void flush() override {}
};
int controlRequest(const char* method,const String& path,const String& payload,ResponseBuffer* output=nullptr) {
    if(WiFi.status()!=WL_CONNECTED)return -1;
    WiFiClient transport;HTTPClient http;
    const String url=String("http://")+settings.host+":"+settings.controlPort+path;
    if(!http.begin(transport,url))return -1;
    http.setConnectTimeout(1000);http.setTimeout(1200);http.setReuse(false);
    http.addHeader("Authorization",String("Bearer ")+settings.token);
    http.addHeader("Content-Type","application/json");
    int status=http.sendRequest(method,payload);
    if(output && status>0) {
        if(http.getSize()>4096 || http.writeToStream(output)<0)status=-2;
    }
    http.end();return status;
}
void closeSession(const char* id) { if(*id)controlRequest("DELETE",String("/api/preview/session/")+id,""); }
void stopLocked() {
    if(!watch.active())return;
    lifecycle.stop();strlcpy(cameraState,"idle",sizeof cameraState);
}
void forgetSession(uint32_t gen) {
    Lock lock(stateMutex);if(!lifecycle.forget(gen))return;
    newRequest();strlcpy(cameraState,"starting",sizeof cameraState);lastDisplayAt=0;lastSequence=0;
}
void controlTask(void*) {
    uint32_t lastStatus=0,lastAttempt=millis()-2000,lastWifiAttempt=millis()-10000;
    for(;;) {
        char retired[97]={};
        { Lock lock(stateMutex);strlcpy(retired,retiredSession,sizeof retired);retiredSession[0]=0; }
        if(*retired)closeSession(retired);
        const uint32_t now=millis();
        if(WiFi.status()!=WL_CONNECTED && now-lastWifiAttempt>=10000) { WiFi.reconnect();lastWifiAttempt=now; }
        auto v=view();
        // Stop is serviced independently of the MJPEG read loop.
        if(!v.active) {
            if(*v.id) {
                { Lock lock(stateMutex);if(v.gen==generation && !watch.active())session[0]=0; }
                closeSession(v.id); // best effort; server lease also bounds disconnected cleanup
            }
            vTaskDelay(pdMS_TO_TICKS(30));continue;
        }
        if(WiFi.status()!=WL_CONNECTED) { setError(v.gen,"Wi-Fi reconnecting");vTaskDelay(pdMS_TO_TICKS(100));continue; }
        if(!*v.id) {
            if(now-lastAttempt<1500) { vTaskDelay(pdMS_TO_TICKS(30));continue; }
            lastAttempt=now;ResponseBuffer response;
            const int code=controlRequest("POST","/api/preview/session",String("{\"request_id\":\"")+v.request+"\"}",&response);
            char created[97]={};char state[40]="starting";
            if(code==202) {
                cJSON* json=cJSON_Parse(response.bytes);
                const auto* id=cJSON_GetObjectItemCaseSensitive(json,"session_id");
                const auto* status=cJSON_GetObjectItemCaseSensitive(json,"state");
                if(cJSON_IsString(id) && safeId(id->valuestring))strlcpy(created,id->valuestring,sizeof created);
                if(cJSON_IsString(status))strlcpy(state,status->valuestring,sizeof state);
                cJSON_Delete(json);
            }
            bool adopted=false;
            {
                Lock lock(stateMutex);
                if(generation==v.gen && watch.active()) {
                    if(*created && lifecycle.adopt(v.gen,created,millis())) { strlcpy(cameraState,state,sizeof cameraState);lastError[0]=0;adopted=true; }
                    else strlcpy(lastError,code==409?"Camera busy":(code==401 || code==403)?"Pairing token rejected":"Camera start/reply failed",sizeof lastError);
                }
            }
            // A late START response may only close the session it created, never a newer one.
            if(*created && !adopted)closeSession(created);
            lastStatus=0;continue;
        }
        if(v.renew && now-lastAttempt>=1000) {
            lastAttempt=now;int code=controlRequest("POST",String("/api/preview/session/")+v.id+"/renew","");
            if(code==404) { forgetSession(v.gen);continue; }
            if(code==204) { Lock lock(stateMutex);if(generation==v.gen) { watch.renewed(millis());lastError[0]=0; } }
            else setError(v.gen,(code==401 || code==403)?"Pairing token rejected":"Renew retry pending");
        }
        if(now-lastStatus>=1000) {
            lastStatus=now;ResponseBuffer response;
            int code=controlRequest("GET",String("/api/preview/status?session_id=")+v.id,"",&response);
            if(code==404) { forgetSession(v.gen);continue; }
            if(code==200) {
                cJSON* json=cJSON_Parse(response.bytes);auto* state=cJSON_GetObjectItemCaseSensitive(json,"state");
                if(cJSON_IsString(state) && strcmp(state->valuestring,"closed")==0) { cJSON_Delete(json);forgetSession(v.gen);continue; }
                { Lock lock(stateMutex);if(generation==v.gen && cJSON_IsString(state)) { strlcpy(cameraState,state->valuestring,sizeof cameraState);lastError[0]=0; } }
                cJSON_Delete(json);
            } else setError(v.gen,(code==401 || code==403)?"Pairing token rejected":"Control reconnecting");
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
struct StreamContext { uint32_t gen;uint64_t sequence=0;bool hasSequence=false;uint64_t completedFrames=0; };
void frameReceived(void* context,const uint8_t* bytes,size_t n,uint64_t sequence) {
    auto& source=*static_cast<StreamContext*>(context);
    ++source.completedFrames;
    if(source.hasSequence && sequence<=source.sequence)return;
    source.sequence=sequence;source.hasSequence=true;
    // No queue: if decoding owns the second JPEG buffer, drop this frame.
    if(xSemaphoreTake(frameMutex,0)!=pdTRUE)return;
    { Lock lock(stateMutex);
        if(watch.active() && generation==source.gen) {
            // Reconnecting to a cached frame must not make an old picture LIVE again.
            if((!lastDisplayAt || sequence>lastSequence) &&
               (!pendingLength || pendingGeneration!=source.gen || sequence>pendingSequence)) {
                memcpy(pending,bytes,n);pendingLength=n;pendingSequence=sequence;pendingGeneration=source.gen;pendingAt=millis();
            }
        }
    }
    xSemaphoreGive(frameMutex);
}
void streamTask(void*) {
    for(;;) {
        auto v=view();
        if(!v.active || !*v.id || WiFi.status()!=WL_CONNECTED) { vTaskDelay(pdMS_TO_TICKS(50));continue; }
        WiFiClient stream;stream.setTimeout(1000);
        if(!stream.connect(settings.host,settings.streamPort,1000)) { setError(v.gen,"Stream reconnecting");vTaskDelay(pdMS_TO_TICKS(700));continue; }
        String request=String("GET /stream?session_id=")+v.id+" HTTP/1.1\r\nHost: "+settings.host+":"+settings.streamPort+"\r\nAuthorization: Bearer "+settings.token+"\r\nAccept: multipart/x-mixed-replace\r\nConnection: close\r\n\r\n";
        if(stream.print(request)!=request.length()) { stream.stop();continue; }
        StreamContext context={v.gen,0,false};MjpegParser parser(incoming,kMaxJpegBytes,frameReceived,&context);
        uint8_t bytes[2048];uint32_t lastByte=millis();FrameDeadline frameDeadline;
        while(stream.connected() || stream.available()) {
            auto current=view();
            if(!current.active || current.gen!=v.gen || strcmp(current.id,v.id)!=0 || WiFi.status()!=WL_CONNECTED)break;
            int available=stream.available();
            if(available>0) {
                int n=stream.read(bytes,std::min(available,int(sizeof bytes)));
                if(n>0) { lastByte=millis();if(!parser.feed(bytes,n)) { setError(v.gen,parser.error());break; } }
            } else vTaskDelay(pdMS_TO_TICKS(5));
            if(frameDeadline.expired(parser.inFrame(),context.completedFrames,millis()))break;
            if(millis()-lastByte>=2500)break;
        }
        stream.stop();vTaskDelay(pdMS_TO_TICKS(300));
    }
}
}
bool previewClientBegin(const ViewerConfig& config) {
    settings=config;configured=config.ready();stateMutex=xSemaphoreCreateMutex();frameMutex=xSemaphoreCreateMutex();
    incoming=static_cast<uint8_t*>(heap_caps_malloc(kMaxJpegBytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    pending=static_cast<uint8_t*>(heap_caps_malloc(kMaxJpegBytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if(!stateMutex || !frameMutex || !incoming || !pending)return false;
    if(!configured)return true;
    WiFi.mode(WIFI_STA);WiFi.setAutoReconnect(true);WiFi.setSleep(true);WiFi.begin(settings.ssid,settings.password);
    TaskHandle_t control=nullptr;
    if(xTaskCreatePinnedToCore(controlTask,"preview-control",8192,nullptr,2,&control,0)!=pdPASS)return false;
    if(xTaskCreatePinnedToCore(streamTask,"preview-stream",6144,nullptr,1,nullptr,0)!=pdPASS) { vTaskDelete(control);return false; }
    return true;
}
void previewStart() {
    if(!configured)return;
    Lock lock(stateMutex);if(!lifecycle.start(millis()))return;
    // A prior session is in retiredSession and is never reused by a new START.
    newRequest();lastDisplayAt=0;lastSequence=0;lastError[0]=0;strlcpy(cameraState,"starting",sizeof cameraState);
}
void previewStop() { Lock lock(stateMutex);stopLocked(); }
void previewExtend() { Lock lock(stateMutex);watch.extend(millis()); }
PreviewSnapshot previewSnapshot() {
    Lock lock(stateMutex);const uint32_t now=millis();
    if(watch.expired(now))stopLocked();
    PreviewSnapshot out;out.active=watch.active();out.configured=configured;out.wifi=WiFi.status()==WL_CONNECTED;
    out.generation=generation;out.remainingMs=watch.remaining(now);out.frameAgeMs=lastDisplayAt?now-lastDisplayAt:0;
    out.stale=!lastDisplayAt || watch.stale(now);out.sequence=lastSequence;
    strlcpy(out.state,cameraState,sizeof out.state);strlcpy(out.error,lastError,sizeof out.error);return out;
}
bool previewDecodeLatest() {
    if(xSemaphoreTake(frameMutex,0)!=pdTRUE)return false;
    bool display=false;
    auto current=view();
    if(pendingLength && current.active && pendingGeneration==current.gen) {
        display=decodeJpeg(pending,pendingLength);
        Lock lock(stateMutex);
        if(generation==pendingGeneration && watch.active() && display) { lastDisplayAt=pendingAt;lastSequence=pendingSequence;watch.frame(pendingAt); }
        else display=false;
    }
    pendingLength=0;xSemaphoreGive(frameMutex);return display;
}
}
