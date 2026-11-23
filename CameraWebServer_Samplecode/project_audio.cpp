#include <Arduino.h>
#include <atomic>
#include <ESP_I2S.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include "esp_heap_caps.h"
#include "miniz.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "project_settings.h"
#include "project_audio.h"
#include "project_ai_alarm.h"
#include "project_transport.h"

namespace {
constexpr uint32_t RATE = 16000;
I2SClass mic(I2S_NUM_0), amp(I2S_NUM_1);
uint8_t *wav = nullptr;
size_t wavSize = 0;
uint8_t *packedWav = nullptr;
size_t packedSize = 0;
bool packingAttempted = false;
SemaphoreHandle_t recorded = nullptr;
volatile bool recording = false;
bool talkStarted = false;
std::atomic<bool> sleepReportedOnSocket{false};
volatile bool networkRunning = false, playbackRunning = false, talkWorkerRunning = false, liveMicRunning = false;
volatile bool microphoneInitialized = false, microphoneReady = false;
volatile bool speakerInitialized = false, speakerReady = false, activeTalkSession = false;
volatile uint32_t talkGeneration = 0;
volatile unsigned long lastTalkContact = 0;
constexpr size_t LIVE_SAMPLES = 640; // 40 ms per packet; no seconds-long speech queue.
struct Frame { int16_t samples[LIVE_SAMPLES]; size_t count; uint32_t generation; };
QueueHandle_t playQueue = nullptr, captureQueue = nullptr;
extern const uint8_t bundleStart[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t bundleEnd[] asm("_binary_x509_crt_bundle_end");

void clearQueue(QueueHandle_t queue) { Frame frame; while(xQueueReceive(queue,&frame,0)==pdTRUE) {} }
void changeCall(bool active) {
  if(active==activeTalkSession) return;
  activeTalkSession=false;++talkGeneration;
  clearQueue(playQueue);clearQueue(captureQueue);
  activeTalkSession=active;
  Serial.println(active ? "INTERCOM ACTIVE: live microphone and speaker; staying awake until End call or Sleep now." : "INTERCOM STOPPED: microphone stream off.");
}

void put16(uint8_t *p, uint16_t x) { p[0] = x; p[1] = x >> 8; }
void put32(uint8_t *p, uint32_t x) { for (int i=0;i<4;++i) p[i]=x>>(i*8); }
void wavHeader(size_t samples) {
  memcpy(wav,"RIFF",4); put32(wav+4,36+samples*2); memcpy(wav+8,"WAVEfmt ",8);
  put32(wav+16,16); put16(wav+20,1);put16(wav+22,1);put32(wav+24,RATE);
  put32(wav+28,RATE*2);put16(wav+32,2);put16(wav+34,16);
  memcpy(wav+36,"data",4);put32(wav+40,samples*2);wavSize=44+samples*2;
}
void recordTask(void *) {
  int32_t raw[256]; size_t samples=0;
  const size_t target=RATE*EVENT_AUDIO_SECONDS;
  unsigned long started=millis();
  // First 100 ms are discarded to let the microphone settle.
  size_t discarded=0;
  while (samples<target && millis()-started<(EVENT_AUDIO_SECONDS*1000+1500)) {
    size_t bytes=0;
    esp_err_t err=i2s_channel_read(mic.rxChan(),raw,sizeof(raw),&bytes,150);
    if (err!=ESP_OK) continue;
    for(size_t i=0;i<bytes/4 && samples<target;++i) {
      if(discarded<RATE/10) {++discarded;continue;}
      int32_t value=(raw[i]/65536)*PROJECT_MIC_GAIN;
      value=constrain(value,-32768,32767);
      put16(wav+44+samples*2,uint16_t(int16_t(value)));++samples;
    }
  }
  mic.end();
  if(samples>=RATE/10) wavHeader(samples);
  else {free(wav);wav=nullptr;wavSize=0;}
  recording=false;xSemaphoreGive(recorded);vTaskDelete(nullptr);
}
bool beginRequest(HTTPClient &http, WiFiClientSecure &client, const String &url) {
  if(!url.length() || WiFi.status()!=WL_CONNECTED || !configureProjectTls(client)) return false;
  http.setConnectTimeout(2500);http.setTimeout(2500);
  if(!http.begin(client,url)) return false;
  addProjectDeviceHeaders(http);
  return true;
}
void playbackTask(void *) {
  amp.setPins(SPEAKER_BCLK_PIN,SPEAKER_WS_PIN,SPEAKER_DATA_PIN,-1);
  speakerReady=amp.begin(I2S_MODE_STD,RATE,I2S_DATA_BIT_WIDTH_16BIT,I2S_SLOT_MODE_STEREO);
  speakerInitialized=true;
  if(!speakerReady) {playbackRunning=false;vTaskDelete(nullptr);return;}
  int16_t stereo[LIVE_SAMPLES*2];Frame frame;
  while(networkRunning) {
    if(xQueueReceive(playQueue,&frame,pdMS_TO_TICKS(30))!=pdTRUE) {
      // Keep the DMA output silent when no speech is available.
      memset(stereo,0,sizeof(stereo));amp.write(stereo,128*4);continue;
    }
    if(!activeTalkSession || frame.generation!=talkGeneration || millis()-lastTalkContact>TALK_CONNECTION_GRACE_MS) continue;
    for(size_t i=0;i<frame.count;i++) stereo[2*i]=stereo[2*i+1]=int16_t(frame.samples[i]*PROJECT_SPEAKER_VOLUME);
    if(amp.write(stereo,frame.count*4)!=frame.count*4) Serial.println("Intercom: speaker write incomplete.");
  }
  memset(stereo,0,sizeof(stereo));amp.write(stereo,sizeof(stereo));amp.end();
  speakerReady=false;playbackRunning=false;vTaskDelete(nullptr);
}
void liveMicTask(void *) {
  mic.setPins(MIC_BCLK_PIN,MIC_WS_PIN,-1,MIC_DATA_PIN);
  microphoneReady=mic.begin(I2S_MODE_STD,RATE,I2S_DATA_BIT_WIDTH_32BIT,I2S_SLOT_MODE_MONO,I2S_STD_SLOT_LEFT);
  microphoneInitialized=true;
  if(!microphoneReady) {liveMicRunning=false;vTaskDelete(nullptr);return;}
  int32_t raw[160];Frame frame={};
  while(networkRunning) {
    size_t bytes=0;
    if(i2s_channel_read(mic.rxChan(),raw,sizeof(raw),&bytes,100)!=ESP_OK) continue;
    if(!activeTalkSession) {frame.count=0;continue;} // No transmission until a browser starts a call.
    if(frame.generation!=talkGeneration) {frame.count=0;frame.generation=talkGeneration;}
    for(size_t i=0;i<bytes/4;i++) {
      int32_t value=(raw[i]/65536)*PROJECT_MIC_GAIN;
      frame.samples[frame.count++]=int16_t(constrain(value,-32768,32767));
      if(frame.count==LIVE_SAMPLES) {
        if(xQueueSend(captureQueue,&frame,0)!=pdTRUE) {
          Frame old;xQueueReceive(captureQueue,&old,0);xQueueSend(captureQueue,&frame,0);
        }
        frame.count=0;
      }
    }
  }
  mic.end();microphoneReady=false;liveMicRunning=false;vTaskDelete(nullptr);
}
// Return from this function before deleting the task so local TLS/socket objects are released.
void runIntercom() {
  unsigned long initializedAt=millis();
  while((!speakerInitialized || !microphoneInitialized) && millis()-initializedAt<5000) delay(10);
  if(!speakerReady || !microphoneReady) {
    Serial.println("Intercom unavailable: microphone or speaker initialization failed.");
    reportProjectAudioState("error");networkRunning=false;
    while(playbackRunning || liveMicRunning) delay(20);
    return;
  }
  String origin=projectServerUrl("");
  // projectServerUrl returns the base when passed an empty suffix.
  if(!origin.startsWith("https://") || time(nullptr)<1700000000) {
    Serial.println("Intercom needs a verified HTTPS address and synchronized clock.");
    networkRunning=false;while(playbackRunning || liveMicRunning) delay(20);
    return;
  }
  String host=origin.substring(8);int slash=host.indexOf('/');if(slash>=0)host=host.substring(0,slash);
  uint16_t port=443;int colon=host.indexOf(':');if(colon>=0){port=host.substring(colon+1).toInt();host=host.substring(0,colon);}
  String headers=String("X-Device-ID: ")+DEVICE_ID+"\r\nAuthorization: Bearer "+DEVICE_UPLOAD_TOKEN;
  unsigned long lastActive=millis(),lastPing=0;
  bool closing=false,requestedSleep=false;
  WebSocketsClient socket;
  socket.onEvent([&](WStype_t type,uint8_t *payload,size_t length) {
    if(type==WStype_CONNECTED) {
      if(closing) return;
      lastTalkContact=millis();
      socket.sendTXT("{\"type\":\"ready\",\"supports_sleep\":true}");
      Serial.println("INTERCOM READY: dashboard can listen and speak.");
    } else if(type==WStype_DISCONNECTED) {
      changeCall(false);
      if(!closing) Serial.println("Intercom connection interrupted; reconnecting.");
    } else if(type==WStype_TEXT && !closing) {
      JsonDocument message;
      if(deserializeJson(message,payload,length))return;
      const char *kind=message["type"] | "";lastTalkContact=millis();
      if(!strcmp(kind,"start")){changeCall(true);lastActive=millis();}
      else if(!strcmp(kind,"stop")){changeCall(false);lastActive=millis();}
      else if(!strcmp(kind,"sleep")) {
        const char *id=message["id"] | "";
        if(strlen(id)!=32 || strspn(id,"0123456789abcdef")!=32) return;
        requestedSleep=closing=true;
        changeCall(false);
        networkRunning=false;
        const bool pending=projectAiAlarmPending();
        char ack[112];
        snprintf(ack,sizeof(ack),"{\"type\":\"sleep_ack\",\"id\":\"%s\",\"state\":\"%s\"}",
                 id,pending ? "waiting_for_alarm" : "closing");
        socket.sendTXT(ack);
        Serial.println(pending ? "Dashboard requested sleep: audio stopped; finishing the current AI alarm check."
                               : "Dashboard requested sleep: closing the intercom and rearming the PIR.");
      }
    } else if(type==WStype_BIN && !closing && activeTalkSession && length>0 && length<=LIVE_SAMPLES*2 && length%2==0) {
      Frame frame={};frame.count=length/2;frame.generation=talkGeneration;memcpy(frame.samples,payload,length);
      if(xQueueSend(playQueue,&frame,0)!=pdTRUE) {Frame old;xQueueReceive(playQueue,&old,0);xQueueSend(playQueue,&frame,0);}
    }
  });
  // Same ESP32 built-in root bundle as HTTPS uploads; hostname verification stays enabled.
  socket.beginSslWithBundle(host.c_str(),port,"/api/device/intercom",bundleStart,bundleEnd-bundleStart,"");
  socket.setExtraHeaders(headers.c_str());socket.setReconnectInterval(2000);socket.enableHeartbeat(10000,5000,2);
  while(!closing && millis()-lastActive<TALK_WINDOW_MS) {
    socket.loop();
    if(closing) break;
    if(activeTalkSession && millis()-lastTalkContact<TALK_CONNECTION_GRACE_MS) lastActive=millis();
    else if(activeTalkSession) changeCall(false);
    if(socket.isConnected() && millis()-lastPing>=4000) {socket.sendTXT("{\"type\":\"ping\"}");lastPing=millis();}
    Frame frame;
    if(xQueueReceive(captureQueue,&frame,0)==pdTRUE && socket.isConnected() && activeTalkSession && frame.generation==talkGeneration) {
      socket.sendBIN((uint8_t*)frame.samples,frame.count*2);
    }
    delay(1);
  }
  closing=true;
  changeCall(false);
  networkRunning=false;
  while(playbackRunning || liveMicRunning) delay(20);
  // The main loop owns the LED/buzzer timer and the outstanding AI result.
  // Keep the accepted request visible while those finish, without reopening the call.
  while(requestedSleep && projectAiAlarmPending() && socket.isConnected()) {
    socket.loop();
    if(socket.isConnected() && millis()-lastPing>=4000) {socket.sendTXT("{\"type\":\"ping\"}");lastPing=millis();}
    delay(20);
  }
  if(socket.isConnected() && !projectAiAlarmPending()) {
    bool sent=socket.sendTXT("{\"type\":\"sleeping\"}");
    socket.loop();delay(100);
    if(sent) sleepReportedOnSocket.store(true);
  }
  socket.disconnect();
}

void networkTask(void *) {
  runIntercom();
  if(playQueue){vQueueDelete(playQueue);playQueue=nullptr;}
  if(captureQueue){vQueueDelete(captureQueue);captureQueue=nullptr;}
  talkWorkerRunning=false;vTaskDelete(nullptr);
}
}

void startProjectAudioRecording() {
  if(!EVENT_AUDIO_ENABLED) return;
  discardProjectAudio();
  if(!recorded) recorded=xSemaphoreCreateBinary();
  if(!recorded) return;
  xSemaphoreTake(recorded,0);
  wav=(uint8_t*)heap_caps_malloc(44+RATE*2*EVENT_AUDIO_SECONDS,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  if(!wav) {Serial.println("Audio skipped: insufficient PSRAM.");return;}
  mic.setPins(MIC_BCLK_PIN,MIC_WS_PIN,-1,MIC_DATA_PIN);
  if(!mic.begin(I2S_MODE_STD,RATE,I2S_DATA_BIT_WIDTH_32BIT,I2S_SLOT_MODE_MONO,I2S_STD_SLOT_LEFT)) {
    free(wav);wav=nullptr;Serial.println("Microphone initialization failed; photos continue.");return;
  }
  recording=true;
  if(xTaskCreate(recordTask,"event-audio",4096,nullptr,2,nullptr)!=pdPASS) {
    recording=false;mic.end();free(wav);wav=nullptr;return;
  }
  Serial.printf("Recording %u seconds of audio alongside the photo burst.\n",EVENT_AUDIO_SECONDS);
}
void finishProjectAudioRecording() {
  if(recording && recorded) xSemaphoreTake(recorded,pdMS_TO_TICKS((EVENT_AUDIO_SECONDS+3)*1000));
}
bool projectAudioPending() {return !recording && wav && wavSize>44;}
bool projectAudioRecording() {return recording;}
size_t projectAudioPendingBytes() {
  if(recording) return 44+RATE*2*EVENT_AUDIO_SECONDS;
  return projectAudioPending() ? wavSize : 0;
}
void discardProjectAudio() {
  finishProjectAudioRecording();
  if(recording) return; // Never free a buffer still owned by the recording task.
  free(wav);wav=nullptr;wavSize=0;
  free(packedWav);packedWav=nullptr;packedSize=0;packingAttempted=false;
}
static void packRecording() {
  // Keep the original until the server confirms storage. Skip compression when
  // it would save less than 5%, or if another component needs the PSRAM.
  packingAttempted=true;
  const size_t capacity=wavSize-wavSize/20;
  auto *encoder=static_cast<tdefl_compressor *>(heap_caps_malloc(sizeof(tdefl_compressor),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
  if(!encoder) return;
  packedWav=static_cast<uint8_t *>(heap_caps_malloc(capacity,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
  unsigned long started=millis();
  if(packedWav && tdefl_init(encoder,nullptr,nullptr,TDEFL_WRITE_ZLIB_HEADER|1)==TDEFL_STATUS_OKAY) {
    size_t consumed=wavSize, produced=capacity;
    if(tdefl_compress(encoder,wav,&consumed,packedWav,&produced,TDEFL_FINISH)==TDEFL_STATUS_DONE && consumed==wavSize)
      packedSize=produced;
  }
  free(encoder);
  if(!packedSize) {free(packedWav);packedWav=nullptr;}
  Serial.printf("Event audio prepared: %u WAV bytes, %u upload bytes in %lu ms.\n",
                unsigned(wavSize),unsigned(packedSize ? packedSize : wavSize),millis()-started);
}
void uploadProjectAudio(const String &event, HTTPClient &http, WiFiClientSecure &client, bool &acceptsDeflate) {
  if(!projectAudioPending()) return;
  if(acceptsDeflate && !packingAttempted) packRecording();
  const bool compressed=acceptsDeflate && packedSize>0;
  bool reused=client.connected();
  addProjectDeviceHeaders(http);
  http.addHeader("Content-Type","audio/wav");http.addHeader("X-Event-ID",event);
  if(compressed) http.addHeader("Content-Encoding","deflate");
  unsigned long start=millis();
  const size_t bytes=compressed ? packedSize : wavSize;
  int code=http.POST(compressed ? packedWav : wav,bytes);logProjectTransportError(client,code);
  Serial.printf("Event audio upload HTTP %d (%u bytes in %lu ms, TLS %s).\n",
                code,unsigned(bytes),millis()-start,reused?"reused":"new connection");
  if(code==200 || code==201) discardProjectAudio();
  else if(compressed && (code==415 || code==422)) {
    acceptsDeflate=false;
    Serial.println("Server rejected the encoding; retrying the original WAV.");
  }
  else if(code==401 || code==403 || code==409 || code==422) {
    Serial.println("Audio rejected: check the camera token and uploaded event. Skipping this recording.");
    discardProjectAudio();
  }
}
bool pollProjectAudioTalk() {
  if(!TALK_ENABLED) return false;
  if(!talkStarted) {
    talkStarted=true;
    if(recording) {Serial.println("Intercom waits for event audio to finish.");talkStarted=false;return true;}
    playQueue=xQueueCreate(8,sizeof(Frame));captureQueue=xQueueCreate(4,sizeof(Frame));
    if(!playQueue || !captureQueue) {reportProjectAudioState("error");return false;}
    networkRunning=playbackRunning=liveMicRunning=true;
    if(xTaskCreate(playbackTask,"speaker",6144,nullptr,2,nullptr)!=pdPASS) {networkRunning=playbackRunning=liveMicRunning=false;reportProjectAudioState("error");return false;}
    if(xTaskCreate(liveMicTask,"live-mic",6144,nullptr,2,nullptr)!=pdPASS) {networkRunning=liveMicRunning=false;reportProjectAudioState("error");return playbackRunning;}
    talkWorkerRunning=true;
    if(xTaskCreate(networkTask,"intercom",12288,nullptr,1,nullptr)!=pdPASS) {networkRunning=talkWorkerRunning=false;return playbackRunning || liveMicRunning;}
    Serial.printf("Live intercom: %lu seconds idle timeout; active calls stay awake until End call.\n",TALK_WINDOW_MS/1000);
  }
  return talkWorkerRunning || playbackRunning || liveMicRunning;
}

void reportProjectAudioState(const char *state) {
  if(sleepReportedOnSocket.load() && !strcmp(state,"sleeping")) return;
  if(WiFi.status()!=WL_CONNECTED) return;
  WiFiClientSecure client;HTTPClient http;
  String url=projectServerUrl("/api/device/talk?ready=false&state=")+state+"&device_id="+DEVICE_ID;
  if(!beginRequest(http,client,url)) return;
  int code=http.GET();logProjectTransportError(client,code);http.end();
  Serial.printf("Camera status: %s (HTTP %d).\n",state,code);
}
