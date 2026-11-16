#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "project_settings.h"
#include "project_sensors.h"
#include "project_sleep.h"
#include "project_ai_alarm.h"
#include "project_audio.h"
#include "project_transport.h"

namespace {
SemaphoreHandle_t buzzerLock = nullptr;
esp_timer_handle_t buzzerTimer = nullptr;
int64_t buzzerDeadline = 0;
void stopBuzzerTimer(void *) {
  if (buzzerLock && xSemaphoreTake(buzzerLock, portMAX_DELAY)) {
    if (esp_timer_get_time() >= buzzerDeadline)
      digitalWrite(BUZZER_PIN, !BUZZER_ACTIVE_LEVEL);
    xSemaphoreGive(buzzerLock);
  }
}
struct Photo { uint8_t *data = nullptr; size_t size = 0; };
Photo pending[BURST_COUNT];
String eventId;
unsigned long lastRetry = 0;
unsigned long uploadStarted = 0;
bool nightEvent = false;
const size_t MAX_PHOTO_BYTES = 1024 * 1024;

void light(bool on) {
  if (WHITE_LED_PIN >= 0)
    digitalWrite(WHITE_LED_PIN, on ? WHITE_LED_ACTIVE_LEVEL : !WHITE_LED_ACTIVE_LEVEL);
}
bool hasPendingPhotos() {
  for (auto &photo : pending) if (photo.data) return true;
  return false;
}
bool hasPending() { return hasPendingPhotos() || projectAudioPending(); }
void discardPending(const char *reason) {
  discardProjectAudio();
  int dropped = 0;
  for (auto &photo : pending) {
    if (photo.data) { free(photo.data); photo = Photo(); ++dropped; }
  }
  if (dropped) Serial.printf("Discarded %d unsent photos: %s. Ready for next trigger.\n", dropped, reason);
}
void uploadPending() {
  lastRetry = millis();
  if (millis() - uploadStarted >= UPLOAD_GIVE_UP_MS) {
    discardPending("upload retry window expired");
    return;
  }
  if (!LOCAL_DASHBOARD_UPLOAD_URL[0] || WiFi.status() != WL_CONNECTED) {
    Serial.println("Photos waiting in RAM: check dashboard URL/Wi-Fi. Will give up after retry window.");
    return;
  }
  for (int i = 0; i < BURST_COUNT; ++i) {
    if (!pending[i].data) continue;
    if (millis() - uploadStarted >= UPLOAD_GIVE_UP_MS) {
      discardPending("upload retry window expired");
      break;
    }

    WiFiClientSecure client;
    if (!configureProjectTls(client)) return;

    HTTPClient http;
    http.setConnectTimeout(UPLOAD_CONNECT_TIMEOUT_MS);
    http.setTimeout(UPLOAD_RESPONSE_TIMEOUT_MS);
    if (!http.begin(client, LOCAL_DASHBOARD_UPLOAD_URL)) break;
    addProjectDeviceHeaders(http);
    http.addHeader("Content-Type", "image/jpeg");
    http.addHeader("X-Event-ID", eventId);
    http.addHeader("X-Photo-Index", String(i + 1));
    http.addHeader("X-Night-Capture", nightEvent ? "true" : "false");
    
    int code = http.POST(pending[i].data, pending[i].size);
    logProjectTransportError(client, code);
    Serial.printf("Photo %d/%d: dashboard HTTP %d\n", i + 1, BURST_COUNT, code);
    
    if (code != 200 && code != 201) {
      Serial.print("Response: ");
      Serial.println(http.getString());
    }

    http.end();
    if (code == 200 || code == 201) {
      free(pending[i].data);
      pending[i] = Photo();
      lastRetry = millis() - UPLOAD_RETRY_MS;
    } else {
      if (code == 401 || code == 403) {
        Serial.println("Upload rejected: check DEVICE_ID and DEVICE_UPLOAD_TOKEN.");
        discardPending("camera credentials rejected; fix the token before retrying");
        return;
      }
      Serial.println("Upload not confirmed; retained for retry. Check dashboard URL/server/firewall.");
      lastRetry = millis();
    }
    // Send one photo per loop so the retry deadline is checked between requests.
    return;
  }
  if (projectAudioPending()) uploadProjectAudio(eventId);
}
void captureBurst() {
  char id[32];
  snprintf(id, sizeof(id), "%08lx-%08lx-%08lx", (unsigned long)esp_random(),
           (unsigned long)esp_random(), millis());
  eventId = id;
  int reading = LIGHT_SENSOR_PIN >= 0 ? analogRead(LIGHT_SENSOR_PIN) : -1;
  nightEvent = reading >= 0 && (DARK_WHEN_READING_LOW ? reading < DARK_THRESHOLD : reading > DARK_THRESHOLD);
  Serial.printf("Burst %s. Light reading: %d. Dark: %s\n", id, reading, nightEvent ? "yes" : "no");
  light(nightEvent);
  // Consume frames while automatic exposure/white balance settle under the
  // actual capture lighting. A delay alone can leave old frames in the queue.
  unsigned long settleStarted = millis();
  int warmupFrames = 0;
  while ((millis() - settleStarted < LIGHT_SETTLE_MS || warmupFrames < CAMERA_WARMUP_FRAMES)
         && millis() - settleStarted < LIGHT_SETTLE_MS + 1500) {
    camera_fb_t *old = esp_camera_fb_get();
    if (!old) { Serial.println("Camera warmup frame unavailable; attempting burst."); break; }
    esp_camera_fb_return(old);
    ++warmupFrames;
    delay(10);
  }
  Serial.printf("Camera settled: %d frames in %lu ms. White light: %s.\n",
                warmupFrames, millis() - settleStarted, nightEvent ? "on" : "off");
  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor && sensor->id.PID == OV5640_PID) {
    int gain = sensor->get_reg(sensor, 0x350A, 0x3FF);
    if (gain >= 0) Serial.printf("Sensor gain before burst: %.2fx\n", gain / 16.0f);
  }
  // Start the five-second recording immediately before the saved photo burst.
  startProjectAudioRecording();
  int captured = 0;
  for (int i = 0; i < BURST_COUNT; ++i) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb && fb->format == PIXFORMAT_JPEG && fb->len <= MAX_PHOTO_BYTES) {
      pending[i].data = (uint8_t *)heap_caps_malloc(fb->len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (pending[i].data) {
        memcpy(pending[i].data, fb->buf, fb->len);
        pending[i].size = fb->len;
        ++captured;
      }
    }
    if (fb) esp_camera_fb_return(fb);
    if (!pending[i].data) {
      Serial.printf("Photo %d failed (camera, non-JPEG, size or PSRAM).\n", i + 1);
      break;
    }
    if (i + 1 < BURST_COUNT) delay(PHOTO_INTERVAL_MS);
  }
  light(false);
  finishProjectAudioRecording();
  if (!captured) discardProjectAudio();
  Serial.printf("Captured %d/%d photos; sending event to cloud dashboard.\n", captured, BURST_COUNT);
  uploadStarted = millis();
  beginProjectAiEvent(eventId, captured);
  lastRetry = millis() - UPLOAD_RETRY_MS; // First attempt on next loop iteration.
}
}

void setupProjectSensors() {
  setupProjectAiAlarm();
  if (BUZZER_PIN >= 0) {
    digitalWrite(BUZZER_PIN, !BUZZER_ACTIVE_LEVEL);
    pinMode(BUZZER_PIN, OUTPUT);
    buzzerLock = xSemaphoreCreateMutex();
    esp_timer_create_args_t timerConfig = {};
    timerConfig.callback = stopBuzzerTimer;
    timerConfig.name = "buzzer-off";
    if (esp_timer_create(&timerConfig, &buzzerTimer) != ESP_OK)
      Serial.println("Buzzer timer unavailable; alarm disabled.");
  }
  if (PIR_PIN >= 0) pinMode(PIR_PIN, INPUT);
  if (LIGHT_SENSOR_PIN >= 0) { pinMode(LIGHT_SENSOR_PIN, INPUT); analogReadResolution(12); }
  if (WHITE_LED_PIN >= 0) { digitalWrite(WHITE_LED_PIN, !WHITE_LED_ACTIVE_LEVEL); pinMode(WHITE_LED_PIN, OUTPUT); }
  Serial.println("PIR capture ready.");
}
bool setProjectBuzzer(bool on) {
  if (BUZZER_PIN < 0 || !buzzerLock || !buzzerTimer) return false;
  if (!xSemaphoreTake(buzzerLock, pdMS_TO_TICKS(100))) return false;
  esp_timer_stop(buzzerTimer);
  buzzerDeadline = 0;
  digitalWrite(BUZZER_PIN, !BUZZER_ACTIVE_LEVEL);
  bool success = true;
  if (on) {
    // An independent timer switches it off even while camera/upload code is busy.
    unsigned long duration = constrain(BUZZER_DURATION_MS, 100UL, 10000UL);
    buzzerDeadline = esp_timer_get_time() + (uint64_t)duration * 1000;
    success = esp_timer_start_once(buzzerTimer, (uint64_t)duration * 1000) == ESP_OK;
    if (success) digitalWrite(BUZZER_PIN, BUZZER_ACTIVE_LEVEL);
  }
  xSemaphoreGive(buzzerLock);
  return success;
}
void captureProjectWakeBurst() {
  captureBurst();
}
void pollProjectSensors() {
  if (hasPending() && millis() - uploadStarted >= UPLOAD_GIVE_UP_MS)
    discardPending("upload retry window expired; sleeping");
  if (hasPending() && millis() - lastRetry >= UPLOAD_RETRY_MS) uploadPending();
  if (!hasPending()) {
    bool talkBusy = pollProjectAudioTalk();
    bool aiBusy = pollProjectAiAlarm();
    if (!talkBusy && !aiBusy) sleepProjectUntilMotion();
  }
}
