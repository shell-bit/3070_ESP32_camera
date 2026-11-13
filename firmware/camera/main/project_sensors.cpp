#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
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
#include "project_camera_manager.h"
#include "project_preview.h"
#include "project_face_identity.h"
#include "project_event_upload.h"

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
struct Photo {
  OwnedJpeg image;
  String metadata;
};
Photo pending[BURST_COUNT];
String eventId;
bool armed = false, triggeredBefore = false;
bool buttonWasDown = false;
bool warmupReported = false;
int previousPirLevel = -1;
unsigned long bootTime, lastTrigger = 0, lastRetry = 0;
unsigned long uploadStarted = 0;
bool nightEvent = false;
bool uploadRejected = false;
camera::PendingTrigger triggerRequest;
const size_t MAX_PHOTO_BYTES = 1024 * 1024;

void light(bool on) {
  if (WHITE_LED_PIN >= 0)
    digitalWrite(WHITE_LED_PIN, on ? WHITE_LED_ACTIVE_LEVEL : !WHITE_LED_ACTIVE_LEVEL);
}
bool hasPendingPhotos() {
  for (auto &photo : pending)
    if (photo.image)
      return true;
  return false;
}
bool hasPending() {
  return hasPendingPhotos() || projectAudioPending();
}
void discardPending(const char *reason) {
  discardProjectAudio();
  int dropped = 0;
  for (auto &photo : pending) {
    if (photo.image) {
      photo = Photo();
      ++dropped;
    }
  }
  if (dropped)
    Serial.printf("Discarded %d unsent photos: %s. Ready for next trigger.\n", dropped, reason);
}
void uploadPending() {
  lastRetry = millis();
  if (millis() - uploadStarted >= UPLOAD_GIVE_UP_MS) {
    discardPending("upload retry window expired");
    return;
  }
  if (uploadRejected)
    return;
  if (!LOCAL_DASHBOARD_UPLOAD_URL[0] || WiFi.status() != WL_CONNECTED) {
    Serial.println(
        "Photos waiting in RAM: check dashboard URL/Wi-Fi. Will give up after retry window.");
    return;
  }
  for (int i = 0; i < BURST_COUNT; ++i) {
    if (!pending[i].image)
      continue;
    if (millis() - uploadStarted >= UPLOAD_GIVE_UP_MS) {
      discardPending("upload retry window expired");
      break;
    }

    auto result = uploadEventFrame(pending[i].image, pending[i].metadata, eventId, i + 1,
                                   nightEvent, UPLOAD_GIVE_UP_MS - (millis() - uploadStarted));
    if (result == UploadResult::Saved) {
      pending[i] = Photo();
      lastRetry = millis() - UPLOAD_RETRY_MS;
    } else if (result == UploadResult::Rejected) {
      uploadRejected = true;
      Serial.println("Event upload rejected. Retaining original JPEG/metadata/audio until the "
                     "bounded event deadline; no V1 fallback or new event ID.");
    } else {
      Serial.println(
          "Upload not confirmed; retained for retry. Check dashboard URL/server/firewall.");
      lastRetry = millis();
    }
    // Recheck sensors between requests instead of blocking on all five uploads.
    return;
  }
  if (projectAudioPending())
    uploadProjectAudio(eventId);
}
bool captureBurst() {
  if (hasPending() || !beginCameraOperation(CameraMode::Event)) {
    Serial.println("Camera/event busy; trigger not accepted.");
    return false;
  }
  setPreviewIdentityBusy(true);
  char id[32];
  snprintf(id, sizeof(id), "%08lx-%08lx-%08lx", (unsigned long)esp_random(),
           (unsigned long)esp_random(), millis());
  eventId = id;
  int reading = LIGHT_SENSOR_PIN >= 0 ? analogRead(LIGHT_SENSOR_PIN) : -1;
  nightEvent =
      reading >= 0 && (DARK_WHEN_READING_LOW ? reading < DARK_THRESHOLD : reading > DARK_THRESHOLD);
  Serial.printf("Burst %s. Light reading: %d. Dark: %s\n", id, reading, nightEvent ? "yes" : "no");
  startProjectAudioRecording();
  light(nightEvent);
  delay(LIGHT_SETTLE_MS);
  // Drain old buffered frames after illumination/exposure changes.
  for (int i = 0; i < 2; ++i) {
    captureCameraJpeg(CameraMode::Event, MAX_PHOTO_BYTES);
  }
  int captured = 0;
  for (int i = 0; i < BURST_COUNT; ++i) {
    pending[i].image = captureCameraJpeg(CameraMode::Event, MAX_PHOTO_BYTES);
    if (pending[i].image)
      ++captured;
    if (!pending[i].image) {
      Serial.printf("Photo %d failed (camera, non-JPEG, size or PSRAM).\n", i + 1);
      break;
    }
    if (i + 1 < BURST_COUNT)
      delay(PHOTO_INTERVAL_MS);
  }
  light(false);
  finishProjectAudioRecording();
  if (!captured)
    discardProjectAudio();
  endCameraOperation(CameraMode::Event);
  Serial.printf("Captured %d/%d photos; sending to local dashboard.\n", captured, BURST_COUNT);
  uploadStarted = millis();
  uploadRejected = false;
  const uint64_t identityDeadline = uint64_t(esp_timer_get_time() / 1000) + 10000;
  for (int i = 0; i < BURST_COUNT; ++i)
    if (pending[i].image)
      pending[i].metadata =
          identifyEventFrame(pending[i].image, eventId, i + 1, nightEvent, identityDeadline);
  setPreviewIdentityBusy(false);
  beginProjectAiEvent(eventId, captured);
  lastRetry = millis() - UPLOAD_RETRY_MS; // First attempt on next loop iteration.
  return true;
}
} // namespace

bool projectEventBusy() {
  return hasPending();
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
  bootTime = millis();
  if (PIR_PIN >= 0)
    pinMode(PIR_PIN, INPUT);
  if (LIGHT_SENSOR_PIN >= 0) {
    pinMode(LIGHT_SENSOR_PIN, INPUT);
    analogReadResolution(12);
  }
  if (WHITE_LED_PIN >= 0) {
    digitalWrite(WHITE_LED_PIN, !WHITE_LED_ACTIVE_LEVEL);
    pinMode(WHITE_LED_PIN, OUTPUT);
  }
  if (TRIGGER_BUTTON_PIN >= 0)
    pinMode(TRIGGER_BUTTON_PIN, INPUT_PULLUP);
  if (LOW_POWER_MODE) {
    Serial.println("PIR wake capture ready. Dashboard manual controls disabled in low-power mode.");
    return;
  }
  Serial.println("Local sensor capture ready. Serial B = test burst; L = light reading.");
  Serial.printf("AUTOMATIC mode: PIR GPIO %d; warmup %lu seconds. No capture button is required.\n",
                PIR_PIN, PIR_WARMUP_MS / 1000);
  Serial.println(
      "PIR arms after warmup and a LOW/inactive reading. Sensor GPIO -1 means disabled.");
}
bool setProjectBuzzer(bool on) {
  if (BUZZER_PIN < 0 || !buzzerLock || !buzzerTimer)
    return false;
  if (!xSemaphoreTake(buzzerLock, pdMS_TO_TICKS(100)))
    return false;
  esp_timer_stop(buzzerTimer);
  buzzerDeadline = 0;
  digitalWrite(BUZZER_PIN, !BUZZER_ACTIVE_LEVEL);
  bool success = true;
  if (on) {
    // An independent timer switches it off even while camera/upload code is busy.
    unsigned long duration = constrain(BUZZER_DURATION_MS, 100UL, 10000UL);
    buzzerDeadline = esp_timer_get_time() + (uint64_t)duration * 1000;
    success = esp_timer_start_once(buzzerTimer, (uint64_t)duration * 1000) == ESP_OK;
    if (success)
      digitalWrite(BUZZER_PIN, BUZZER_ACTIVE_LEVEL);
  }
  xSemaphoreGive(buzzerLock);
  return success;
}
void captureProjectWakeBurst() {
  captureBurst();
}
void pollProjectSensors() {
  if (LOW_POWER_MODE) {
    if (hasPending() && millis() - uploadStarted >= UPLOAD_GIVE_UP_MS)
      discardPending("upload retry window expired; sleeping");
    if (hasPending() && millis() - lastRetry >= UPLOAD_RETRY_MS)
      uploadPending();
    if (!hasPending()) {
      bool talkBusy = pollProjectAudioTalk();
      bool aiBusy = pollProjectAiAlarm();
      if (!talkBusy && !aiBusy)
        sleepProjectUntilMotion();
    }
    return;
  }
  if (!hasPending())
    pollProjectAudioTalk();
  bool manual = false;
  bool aiBusy = !hasPending() && pollProjectAiAlarm();
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'b' || c == 'B')
      manual = true;
    if (c == 'l' || c == 'L')
      Serial.printf("Light AO: %d\n", LIGHT_SENSOR_PIN >= 0 ? analogRead(LIGHT_SENSOR_PIN) : -1);
  }
  bool down = TRIGGER_BUTTON_PIN >= 0 && digitalRead(TRIGGER_BUTTON_PIN) == LOW;
  if (down && !buttonWasDown) {
    delay(30);
    if (digitalRead(TRIGGER_BUTTON_PIN) == LOW)
      manual = true;
  }
  buttonWasDown = down;
  bool motion = PIR_PIN >= 0 && digitalRead(PIR_PIN) == PIR_ACTIVE_LEVEL;
  if (PIR_PIN >= 0 && int(motion) != previousPirLevel) {
    Serial.println(motion ? "PIR: ACTIVE (motion)" : "PIR: INACTIVE (clear)");
    previousPirLevel = int(motion);
  }
  bool warmed = millis() - bootTime >= PIR_WARMUP_MS;
  if (warmed && !warmupReported) {
    warmupReported = true;
    Serial.println("PIR warmup complete. Waiting for inactive signal before arming.");
  }
  bool cooled = !triggeredBefore || millis() - lastTrigger >= TRIGGER_COOLDOWN_MS;
  if (PIR_PIN >= 0 && warmed && !motion && cooled && !armed) {
    armed = true;
    Serial.println("AUTO READY: move in front of the PIR to capture 5 photos.");
  }
  uint64_t requestNow = esp_timer_get_time() / 1000;
  if (cooled && (manual || (warmed && armed && motion)))
    triggerRequest.request(requestNow);
  if (triggerRequest.expired(requestNow)) {
    Serial.println(
        "Trigger queue deadline expired while camera/event busy; current evidence preserved.");
    triggerRequest.consumed();
    armed = false;
  }
  if (triggerRequest.pending && !hasPending() && !aiBusy) {
    if (captureBurst()) {
      triggerRequest.consumed();
      armed = false;
      triggeredBefore = true;
      lastTrigger = millis();
      return;
    }
    // A timed-out camera lock has not consumed the PIR/manual request.
  }
  if (hasPending() && millis() - uploadStarted >= UPLOAD_GIVE_UP_MS)
    discardPending("upload retry window expired");
  if (hasPending() && millis() - lastRetry >= UPLOAD_RETRY_MS)
    uploadPending();
}
