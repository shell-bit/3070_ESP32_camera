#include <Arduino.h>
#include <ESP_I2S.h>
#include <HTTPClient.h>
#include <WiFi.h>
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
volatile bool networkRunning = false, playbackRunning = false, talkWorkerRunning = false;
unsigned long talkStart = 0;
struct Frame {
  uint8_t *data;
  size_t bytes;
  char id[33];
};
struct Ack {
  char id[33];
  bool ok;
};
QueueHandle_t playQueue = nullptr, ackQueue = nullptr;

void put16(uint8_t *p, uint16_t x) {
  p[0] = x;
  p[1] = x >> 8;
}
void put32(uint8_t *p, uint32_t x) {
  for (int i = 0; i < 4; ++i)
    p[i] = x >> (i * 8);
}
void wavHeader(size_t samples) {
  memcpy(wav, "RIFF", 4);
  put32(wav + 4, 36 + samples * 2);
  memcpy(wav + 8, "WAVEfmt ", 8);
  put32(wav + 16, 16);
  put16(wav + 20, 1);
  put16(wav + 22, 1);
  put32(wav + 24, RATE);
  put32(wav + 28, RATE * 2);
  put16(wav + 32, 2);
  put16(wav + 34, 16);
  memcpy(wav + 36, "data", 4);
  put32(wav + 40, samples * 2);
  wavSize = 44 + samples * 2;
}
void recordTask(void *) {
  int32_t raw[256];
  size_t samples = 0;
  const size_t target = RATE * EVENT_AUDIO_SECONDS;
  unsigned long started = millis();
  // First 100 ms are discarded to let the microphone settle.
  size_t discarded = 0;
  while (samples < target && millis() - started < (EVENT_AUDIO_SECONDS * 1000 + 1500)) {
    size_t bytes = 0;
    esp_err_t err = i2s_channel_read(mic.rxChan(), raw, sizeof(raw), &bytes, 150);
    if (err != ESP_OK)
      continue;
    for (size_t i = 0; i < bytes / 4 && samples < target; ++i) {
      if (discarded < RATE / 10) {
        ++discarded;
        continue;
      }
      int32_t value = (raw[i] / 65536) * PROJECT_MIC_GAIN;
      value = constrain(value, -32768, 32767);
      put16(wav + 44 + samples * 2, uint16_t(int16_t(value)));
      ++samples;
    }
  }
  mic.end();
  if (samples >= RATE / 10)
    wavHeader(samples);
  else {
    free(wav);
    wav = nullptr;
    wavSize = 0;
  }
  recording = false;
  xSemaphoreGive(recorded);
  vTaskDelete(nullptr);
}
bool beginRequest(HTTPClient &http, WiFiClientSecure &client, const String &url) {
  if (!url.length() || WiFi.status() != WL_CONNECTED || !configureProjectTls(client))
    return false;
  http.setConnectTimeout(2500);
  http.setTimeout(2500);
  if (!http.begin(client, url))
    return false;
  if (DEVICE_UPLOAD_TOKEN[0])
    http.addHeader("Authorization", String("Bearer ") + DEVICE_UPLOAD_TOKEN);
  http.addHeader("ngrok-skip-browser-warning", "1");
  http.addHeader("X-Device-ID", DEVICE_ID);
  return true;
}
void playbackTask(void *) {
  amp.setPins(SPEAKER_BCLK_PIN, SPEAKER_WS_PIN, SPEAKER_DATA_PIN, -1);
  bool ready = amp.begin(I2S_MODE_STD, RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  int16_t stereo[512];
  Frame frame;
  while (networkRunning || uxQueueMessagesWaiting(playQueue) > 0) {
    if (xQueueReceive(playQueue, &frame, pdMS_TO_TICKS(100)) != pdTRUE)
      continue;
    bool ok = ready && (millis() - talkStart < TALK_WINDOW_MS || !LOW_POWER_MODE);
    for (size_t offset = 0; ok && offset < frame.bytes; offset += 512) {
      size_t count = min(size_t(256), (frame.bytes - offset) / 2);
      for (size_t i = 0; i < count; ++i) {
        int16_t value = int16_t(uint16_t(frame.data[offset + 2 * i]) |
                                uint16_t(frame.data[offset + 2 * i + 1]) << 8);
        value = int16_t(value * PROJECT_SPEAKER_VOLUME);
        stereo[2 * i] = stereo[2 * i + 1] = value;
      }
      ok = amp.write(stereo, count * 4) == count * 4;
    }
    // Ensure the final queued DMA samples reach the amplifier before acknowledgement.
    if (ok) {
      memset(stereo, 0, sizeof(stereo));
      amp.write(stereo, sizeof(stereo));
      delay(100);
    }
    Ack ack = {};
    strlcpy(ack.id, frame.id, sizeof(ack.id));
    ack.ok = ok;
    xQueueSend(ackQueue, &ack, 0);
    free(frame.data);
  }
  if (ready)
    amp.end();
  playbackRunning = false;
  vTaskDelete(nullptr);
}
void acknowledge(const Ack &ack) {
  WiFiClientSecure client;
  HTTPClient http;
  if (!beginRequest(http, client, projectServerUrl("/api/device/talk/ack")))
    return;
  http.addHeader("X-Talk-ID", ack.id);
  http.addHeader("X-Playback-OK", ack.ok ? "true" : "false");
  http.POST("");
  http.end();
}
void networkTask(void *) {
  while (!LOW_POWER_MODE || millis() - talkStart < TALK_WINDOW_MS) {
    Ack ack;
    while (xQueueReceive(ackQueue, &ack, 0) == pdTRUE)
      acknowledge(ack);
    if (uxQueueSpacesAvailable(playQueue) == 0) {
      delay(50);
      continue;
    }
    WiFiClientSecure client;
    HTTPClient http;
    String url = projectServerUrl("/api/device/talk?device_id=") + DEVICE_ID;
    if (!beginRequest(http, client, url)) {
      delay(1000);
      continue;
    }
    const char *keys[] = {"X-Talk-ID"};
    http.collectHeaders(keys, 1);
    int code = http.GET();
    int length = http.getSize();
    if (code == 200 && length > 0 && length <= 32000 && length % 2 == 0 &&
        http.header("X-Talk-ID").length() == 32) {
      Frame frame = {};
      frame.bytes = length;
      strlcpy(frame.id, http.header("X-Talk-ID").c_str(), sizeof(frame.id));
      frame.data = (uint8_t *)heap_caps_malloc(length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      size_t received = 0;
      unsigned long start = millis();
      auto *input = http.getStreamPtr();
      while (frame.data && received < size_t(length) && millis() - start < 3000) {
        int available = input->available();
        if (available > 0) {
          int n =
              input->read(frame.data + received, min(size_t(available), size_t(length) - received));
          if (n > 0)
            received += n;
        } else if (!http.connected())
          break;
        else
          delay(2);
      }
      if (received != size_t(length) || !frame.data || xQueueSend(playQueue, &frame, 0) != pdTRUE) {
        free(frame.data);
        Ack failed = {};
        strlcpy(failed.id, frame.id, 33);
        failed.ok = false;
        xQueueSend(ackQueue, &failed, 0);
      }
    } else if (code == 401 || code == 403) {
      Serial.println("Audio talk unavailable: assign the camera and set its device token.");
      http.end();
      break;
    }
    http.end();
    delay(code == 200 ? 20 : 400);
  }
  WiFiClientSecure client;
  HTTPClient http;
  if (beginRequest(http, client,
                   projectServerUrl("/api/device/talk?ready=false&device_id=") + DEVICE_ID)) {
    http.GET();
    http.end();
  }
  networkRunning = false;
  while (playbackRunning)
    delay(20);
  Ack ack;
  while (xQueueReceive(ackQueue, &ack, 0) == pdTRUE)
    acknowledge(ack);
  talkWorkerRunning = false;
  vTaskDelete(nullptr);
}
} // namespace

void startProjectAudioRecording() {
  if (!EVENT_AUDIO_ENABLED)
    return;
  discardProjectAudio();
  if (!recorded)
    recorded = xSemaphoreCreateBinary();
  if (!recorded)
    return;
  xSemaphoreTake(recorded, 0);
  wav = (uint8_t *)heap_caps_malloc(44 + RATE * 2 * EVENT_AUDIO_SECONDS,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!wav) {
    Serial.println("Audio skipped: insufficient PSRAM.");
    return;
  }
  mic.setPins(MIC_BCLK_PIN, MIC_WS_PIN, -1, MIC_DATA_PIN);
  if (!mic.begin(I2S_MODE_STD, RATE, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO,
                 I2S_STD_SLOT_LEFT)) {
    free(wav);
    wav = nullptr;
    Serial.println("Microphone initialization failed; photos continue.");
    return;
  }
  recording = true;
  if (xTaskCreate(recordTask, "event-audio", 4096, nullptr, 2, nullptr) != pdPASS) {
    recording = false;
    mic.end();
    free(wav);
    wav = nullptr;
    return;
  }
  Serial.printf("Recording %u seconds of audio alongside the photo burst.\n", EVENT_AUDIO_SECONDS);
}
void finishProjectAudioRecording() {
  if (recording && recorded)
    xSemaphoreTake(recorded, pdMS_TO_TICKS((EVENT_AUDIO_SECONDS + 3) * 1000));
}
bool projectAudioPending() {
  return !recording && wav && wavSize > 44;
}
void discardProjectAudio() {
  finishProjectAudioRecording();
  if (recording)
    return; // Never free a buffer still owned by the recording task.
  free(wav);
  wav = nullptr;
  wavSize = 0;
}
void uploadProjectAudio(const String &event) {
  if (!projectAudioPending())
    return;
  WiFiClientSecure client;
  HTTPClient http;
  if (!beginRequest(http, client, projectServerUrl("/api/device/audio")))
    return;
  http.setTimeout(UPLOAD_RESPONSE_TIMEOUT_MS);
  http.addHeader("Content-Type", "audio/wav");
  http.addHeader("X-Event-ID", event);
  int code = http.POST(wav, wavSize);
  http.end();
  Serial.printf("Event audio upload HTTP %d (%u bytes).\n", code, unsigned(wavSize));
  if (code == 200 || code == 201)
    discardProjectAudio();
  else if (code == 401 || code == 403 || code == 409 || code == 422) {
    Serial.println("Audio rejected: check camera assignment/token and uploaded event. Skipping "
                   "this recording.");
    discardProjectAudio();
  }
}
bool pollProjectAudioTalk() {
  if (!TALK_ENABLED)
    return false;
  if (!talkStarted) {
    talkStarted = true;
    talkStart = millis();
    playQueue = xQueueCreate(2, sizeof(Frame));
    ackQueue = xQueueCreate(4, sizeof(Ack));
    if (!playQueue || !ackQueue)
      return false;
    networkRunning = playbackRunning = true;
    if (xTaskCreate(playbackTask, "speaker", 4096, nullptr, 2, nullptr) != pdPASS) {
      networkRunning = playbackRunning = false;
      return false;
    }
    talkWorkerRunning = true;
    if (xTaskCreate(networkTask, "talk-network", 8192, nullptr, 1, nullptr) != pdPASS) {
      networkRunning = talkWorkerRunning = false;
      return playbackRunning;
    }
    Serial.printf("Dashboard talk window: %lu seconds in low-power mode.\n", TALK_WINDOW_MS / 1000);
  }
  return talkWorkerRunning || playbackRunning;
}
