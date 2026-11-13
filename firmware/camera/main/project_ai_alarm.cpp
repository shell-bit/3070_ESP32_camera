#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "project_settings.h"
#include "project_sensors.h"
#include "project_ai_alarm.h"
#include "project_transport.h"
#include "project_core.h"

namespace {
String currentEvent;
int expectedPhotos = 0;
bool eventPending = false, waiting = false, sounding = false;
unsigned long waitStarted = 0, lastPoll = 0, alarmStarted = 0;

void redLight(bool on) {
  if (RED_LED_PIN >= 0)
    digitalWrite(RED_LED_PIN, on ? RED_LED_ACTIVE_LEVEL : !RED_LED_ACTIVE_LEVEL);
}
bool finish(const char *message) {
  redLight(false);
  if (sounding)
    setProjectBuzzer(false);
  sounding = waiting = eventPending = false;
  Serial.println(message);
  return false;
}
String encodeQuery(const String &value) {
  const char hex[] = "0123456789ABCDEF";
  String encoded;
  for (size_t i = 0; i < value.length(); ++i) {
    unsigned char c = value[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
        c == '_')
      encoded += (char)c;
    else {
      encoded += '%';
      encoded += hex[c >> 4];
      encoded += hex[c & 15];
    }
  }
  return encoded;
}
} // namespace

void setupProjectAiAlarm() {
  if (RED_LED_PIN >= 0) {
    redLight(false);
    pinMode(RED_LED_PIN, OUTPUT);
  }
}

void beginProjectAiEvent(const String &event, int capturedCount) {
  finish("AI alarm: preparing current event.");
  currentEvent = event;
  expectedPhotos = BURST_COUNT;
  eventPending = AI_ALARM_ENABLED && capturedCount > 0;
}

bool pollProjectAiAlarm() {
  if (!eventPending)
    return false;
  unsigned long now = millis();
  if (sounding) {
    unsigned long duration = constrain(BUZZER_DURATION_MS, 100UL, 10000UL);
    if (now - alarmStarted >= duration)
      return finish("AI ALARM complete: buzzer and red LED off.");
    redLight(((now - alarmStarted) / 250) % 2 == 0);
    return true;
  }
  if (!waiting) {
    waiting = true;
    waitStarted = now;
    lastPoll = now - AI_POLL_INTERVAL_MS;
    Serial.printf("AI WAIT: event %s, up to %lu seconds.\n", currentEvent.c_str(),
                  AI_RESULT_WAIT_MS / 1000);
  }
  if (now - waitStarted >= AI_RESULT_WAIT_MS)
    return finish("AI wait expired: result unknown; no alarm. Check dashboard.");
  if (now - lastPoll < AI_POLL_INTERVAL_MS)
    return true;
  lastPoll = now;
  if (WiFi.status() != WL_CONNECTED)
    return true;

  String url = LOCAL_DASHBOARD_UPLOAD_URL;
  const String suffix = "/api/device/upload";
  if (!url.endsWith(suffix))
    return finish("AI alarm disabled: upload URL must end with /api/device/upload.");
  url.remove(url.length() - suffix.length());
  url += "/api/device/event-status?device_id=" + encodeQuery(String(DEVICE_ID));
  url += "&event_id=" + encodeQuery(currentEvent);
  url += "&expected=" + String(expectedPhotos);

  WiFiClientSecure client;
  if (!configureProjectTls(client))
    return true;
  client.setHandshakeTimeout(5);
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(3000);
  if (!http.begin(client, url))
    return true;
  if (DEVICE_UPLOAD_TOKEN[0])
    http.addHeader("Authorization", String("Bearer ") + DEVICE_UPLOAD_TOKEN);
  http.addHeader("Cache-Control", "no-cache");
  http.addHeader("ngrok-skip-browser-warning", "1");
  int code = http.GET();
  String status;
  // The matching endpoint returns a short response with Content-Length.
  if (code == 200 && http.getSize() >= 0 && http.getSize() <= 32)
    status = http.getString();
  http.end();
  status.trim();
  Serial.printf("AI status: HTTP %d %s\n", code, status.c_str());
  if (code == 401 || code == 403)
    return finish("AI authentication rejected: check DEVICE_UPLOAD_TOKEN.");
  if (code != 200)
    return true;
  if (millis() - waitStarted >= AI_RESULT_WAIT_MS)
    return finish("AI wait expired during request: no alarm.");
  if (status == "suspicious") {
    sounding = true;
    alarmStarted = millis();
    redLight(true);
    bool buzzerOk = setProjectBuzzer(true);
    Serial.printf("AI ALARM: current event suspicious; red LED flashing; buzzer %s.\n",
                  buzzerOk ? "on" : "unavailable");
    return true;
  }
  if (status == "normal")
    return finish("AI result normal: no alarm.");
  if (status == "review")
    return finish("AI result needs manual review: no alarm.");
  if (camera::parseAlarm(status.c_str()) == camera::AlarmState::Exempted)
    return finish("AI result family exempted: no alarm.");
  // pending, not_found, invalid response or unknown status: wait within the limit.
  return true;
}
