#include <Arduino.h>
#include <ESP_I2S.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include "esp_heap_caps.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "project_settings.h"
#include "project_audio.h"
#include "project_transport.h"

namespace {
constexpr uint32_t RATE = 16000;
I2SClass mic(I2S_NUM_0), amp(I2S_NUM_1);
uint8_t *wav = nullptr;
size_t wavSize = 0;
SemaphoreHandle_t recorded = nullptr;
volatile bool recording = false;
bool talkStarted = false;
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
  Serial.println(active ? "INTERCOM ACTIVE: live microphone and speaker; staying awake until End call." : "INTERCOM STOPPED: microphone stream off; idle sleep timer restarted.");
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
void networkTask(void *) {
  unsigned long initializedAt=millis();
  while((!speakerInitialized || !microphoneInitialized) && millis()-initializedAt<5000) delay(10);
  if(!speakerReady || !microphoneReady) {
    Serial.println("Intercom unavailable: microphone or speaker initialization failed.");
    reportProjectAudioState("error");networkRunning=false;
    while(playbackRunning || liveMicRunning) delay(20);
    talkWorkerRunning=false;vTaskDelete(nullptr);return;
  }
  String origin=projectServerUrl("");
  // projectServerUrl returns the base when passed an empty suffix.
  if(!origin.startsWith("https://") || time(nullptr)<1700000000) {
    Serial.println("Intercom needs a verified HTTPS address and synchronized clock.");
    networkRunning=false;while(playbackRunning || liveMicRunning) delay(20);
    talkWorkerRunning=false;vTaskDelete(nullptr);return;
  }
  String host=origin.substring(8);int slash=host.indexOf('/');if(slash>=0)host=host.substring(0,slash);
  uint16_t port=443;int colon=host.indexOf(':');if(colon>=0){port=host.substring(colon+1).toInt();host=host.substring(0,colon);}
  WebSocketsClient socket;
  String headers=String("X-Device-ID: ")+DEVICE_ID+"\r\nAuthorization: Bearer "+DEVICE_UPLOAD_TOKEN;
  unsigned long lastActive=millis(),lastPing=0;
  socket.onEvent([&](WStype_t type,uint8_t *payload,size_t length) {
    if(type==WStype_CONNECTED) {
      lastTalkContact=millis();
      socket.sendTXT("{\"type\":\"ready\"}");
      Serial.println("INTERCOM READY: dashboard can listen and speak.");
    } else if(type==WStype_DISCONNECTED) {
      changeCall(false);Serial.println("Intercom connection interrupted; reconnecting.");
    } else if(type==WStype_TEXT) {
      JsonDocument message;
      if(deserializeJson(message,payload,length))return;
      const char *kind=message["type"] | "";lastTalkContact=millis();
      if(!strcmp(kind,"start")){changeCall(true);lastActive=millis();}
      else if(!strcmp(kind,"stop")){changeCall(false);lastActive=millis();}
    } else if(type==WStype_BIN && activeTalkSession && length>0 && length<=LIVE_SAMPLES*2 && length%2==0) {
      Frame frame={};frame.count=length/2;frame.generation=talkGeneration;memcpy(frame.samples,payload,length);
      if(xQueueSend(playQueue,&frame,0)!=pdTRUE) {Frame old;xQueueReceive(playQueue,&old,0);xQueueSend(playQueue,&frame,0);}
    }
  });
  // Same ESP32 built-in root bundle as HTTPS uploads; hostname verification stays enabled.
  socket.beginSslWithBundle(host.c_str(),port,"/api/device/intercom",bundleStart,bundleEnd-bundleStart,"");
  socket.setExtraHeaders(headers.c_str());socket.setReconnectInterval(2000);socket.enableHeartbeat(10000,5000,2);
  while(millis()-lastActive<TALK_WINDOW_MS) {
    socket.loop();
    if(activeTalkSession && millis()-lastTalkContact<TALK_CONNECTION_GRACE_MS) lastActive=millis();
    else if(activeTalkSession) changeCall(false);
    if(socket.isConnected() && millis()-lastPing>=4000) {socket.sendTXT("{\"type\":\"ping\"}");lastPing=millis();}
    Frame frame;
    if(xQueueReceive(captureQueue,&frame,0)==pdTRUE && socket.isConnected() && activeTalkSession && frame.generation==talkGeneration) {
      socket.sendBIN((uint8_t*)frame.samples,frame.count*2);
    }
    delay(1);
  }
  changeCall(false);
  if(socket.isConnected()){socket.sendTXT("{\"type\":\"sleeping\"}");socket.loop();delay(100);}
  socket.disconnect();networkRunning=false;
  while(playbackRunning || liveMicRunning) delay(20);
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
void discardProjectAudio() {
  finishProjectAudioRecording();
  if(recording) return; // Never free a buffer still owned by the recording task.
  free(wav);wav=nullptr;wavSize=0;
}
void uploadProjectAudio(const String &event) {
  if(!projectAudioPending()) return;
  WiFiClientSecure client;HTTPClient http;
  if(!beginRequest(http,client,projectServerUrl("/api/device/audio"))) return;
  http.setTimeout(UPLOAD_RESPONSE_TIMEOUT_MS);
  http.addHeader("Content-Type","audio/wav");http.addHeader("X-Event-ID",event);
  int code=http.POST(wav,wavSize);logProjectTransportError(client,code);http.end();
  Serial.printf("Event audio upload HTTP %d (%u bytes).\n",code,unsigned(wavSize));
  if(code==200 || code==201) discardProjectAudio();
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
  if(WiFi.status()!=WL_CONNECTED) return;
  WiFiClientSecure client;HTTPClient http;
  String url=projectServerUrl("/api/device/talk?ready=false&state=")+state+"&device_id="+DEVICE_ID;
  if(!beginRequest(http,client,url)) return;
  int code=http.GET();logProjectTransportError(client,code);http.end();
  Serial.printf("Camera status: %s (HTTP %d).\n",state,code);
}
