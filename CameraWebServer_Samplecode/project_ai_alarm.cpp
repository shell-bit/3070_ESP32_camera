#include <Arduino.h>
#include <atomic>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "driver/gpio.h"
#include "esp_timer.h"
#include "project_settings.h"
#include "project_ai_alarm.h"
#include "project_transport.h"

namespace {
String currentEvent;
int expectedPhotos = 0;
// The intercom task reads this while the main loop polls the current event.
std::atomic<bool> eventPending{false};
bool waiting = false;
enum class Alarm { Off, Suspicious, Demo };
Alarm sounding = Alarm::Off;
unsigned long waitStarted = 0, lastPoll = 0, alarmStarted = 0;

esp_timer_handle_t pulseTimer = nullptr;
portMUX_TYPE pulseLock = portMUX_INITIALIZER_UNLOCKED;
int64_t pulseStartedUs = -1;
constexpr int64_t pulseUs = int64_t(ALARM_PULSE_MS) * 1000;
constexpr int64_t durationUs = int64_t(ALARM_DURATION_MS) * 1000;
static_assert(pulseUs > 0 && durationUs % pulseUs == 0, "Alarm duration must contain whole pulse phases");

void driveAlarm(bool on) {
  if (RED_LED_PIN >= 0)
    gpio_set_level(gpio_num_t(RED_LED_PIN), on ? RED_LED_ACTIVE_LEVEL : !RED_LED_ACTIVE_LEVEL);
  if (BUZZER_PIN >= 0)
    gpio_set_level(gpio_num_t(BUZZER_PIN), on ? BUZZER_ACTIVE_LEVEL : !BUZZER_ACTIVE_LEVEL);
}

void tickAlarm(void *) {
  portENTER_CRITICAL(&pulseLock);
  if (pulseStartedUs >= 0) {
    const int64_t elapsed = esp_timer_get_time() - pulseStartedUs;
    driveAlarm(elapsed < durationUs && (elapsed / pulseUs) % 2 == 0);
  }
  portEXIT_CRITICAL(&pulseLock);
}

bool startPulsing() {
  if (!pulseTimer) return false;
  portENTER_CRITICAL(&pulseLock);
  pulseStartedUs = esp_timer_get_time();
  driveAlarm(true);
  portEXIT_CRITICAL(&pulseLock);
  if (esp_timer_start_periodic(pulseTimer, pulseUs) == ESP_OK) return true;
  portENTER_CRITICAL(&pulseLock);
  pulseStartedUs = -1;
  driveAlarm(false);
  portEXIT_CRITICAL(&pulseLock);
  return false;
}
bool finish(const char *message) {
  if (sounding != Alarm::Off && pulseTimer) esp_timer_stop(pulseTimer);
  portENTER_CRITICAL(&pulseLock);
  pulseStartedUs = -1;
  driveAlarm(false);
  portEXIT_CRITICAL(&pulseLock);
  sounding = Alarm::Off;
  waiting = false;
  eventPending.store(false);
  Serial.println(message);
  return false;
}
String encodeQuery(const String &value) {
  const char hex[] = "0123456789ABCDEF";
  String encoded;
  for (size_t i = 0; i < value.length(); ++i) {
    unsigned char c = value[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_') encoded += (char)c;
    else { encoded += '%'; encoded += hex[c >> 4]; encoded += hex[c & 15]; }
  }
  return encoded;
}

String readStatusWord(HTTPClient &http, WiFiClientSecure &client) {
  constexpr int maxSize = 32;
  const unsigned long started = millis();
  auto nextByte = [&]() -> int {
    while (millis() - started < 3000) {
      if (client.available()) return client.read();
      if (!client.connected()) break;
      delay(1);
    }
    return -1;
  };

  String result;
  auto readData = [&](int count) {
    if (count < 0 || count > maxSize - int(result.length())) return false;
    for (int i = 0; i < count; ++i) {
      int c = nextByte();
      if ((c < 'a' || c > 'z') && c != '_') return false;
      if (!result.concat(char(c))) return false;
    }
    return true;
  };

  if (!http.header("Transfer-Encoding").equalsIgnoreCase("chunked"))
    return readData(http.getSize()) ? result : String();
  if (http.getSize() >= 0) return String();

  // Chunk lengths, extensions and trailers are framing, separate from the word.
  unsigned framingBytes = 0;
  auto readLine = [&](String &line) {
    line = "";
    while (++framingBytes < 512) {
      int c = nextByte();
      if (c == '\r') return nextByte() == '\n';
      if (c < 32 || c > 126 || line.length() >= 96 || !line.concat(char(c))) return false;
    }
    return false;
  };
  String line;
  while (readLine(line)) {
    int end = line.indexOf(';');
    if (end < 0) end = line.length();
    if (!end) return String();
    int size = 0;
    for (int i = 0; i < end; ++i) {
      char c = line[i];
      if (c >= '0' && c <= '9') size = size * 16 + c - '0';
      else if (c >= 'a' && c <= 'f') size = size * 16 + c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') size = size * 16 + c - 'A' + 10;
      else return String();
      if (size > maxSize) return String();
    }
    if (!size) {
      while (readLine(line)) {
        if (line.isEmpty()) return result;
        if (line.indexOf(':') <= 0) break;
      }
      return String();
    }
    if (!readData(size) || nextByte() != '\r' || nextByte() != '\n') break;
  }
  return String();
}
}

void setupProjectAiAlarm() {
  if (RED_LED_PIN >= 0) pinMode(RED_LED_PIN, OUTPUT);
  if (BUZZER_PIN >= 0) pinMode(BUZZER_PIN, OUTPUT);
  driveAlarm(false);
  esp_timer_create_args_t timerConfig = {};
  timerConfig.callback = tickAlarm;
  timerConfig.skip_unhandled_events = true;
  timerConfig.name = "alarm-pulse";
  if (esp_timer_create(&timerConfig, &pulseTimer) != ESP_OK)
    Serial.println("Alarm pulse timer unavailable.");
}

void beginProjectAiEvent(const String &event, int capturedCount) {
  finish("AI alarm: preparing current event.");
  currentEvent = event;
  expectedPhotos = capturedCount;
  eventPending = AI_ALARM_ENABLED && capturedCount > 0;
}

bool projectAiAlarmPending() {
  return eventPending.load();
}

bool pollProjectAiAlarm() {
  if (!eventPending) return false;
  unsigned long now = millis();
  if (sounding != Alarm::Off) {
    if (now - alarmStarted >= ALARM_DURATION_MS)
      return finish(sounding == Alarm::Demo ? "DEMO ALARM complete: buzzer and red LED off." :
                                              "AI ALARM complete: buzzer and red LED off.");
    return true;
  }
  if (!waiting) {
    waiting = true;
    waitStarted = now;
    lastPoll = now - AI_POLL_INTERVAL_MS;
    Serial.printf("AI WAIT: event %s, up to %lu seconds.\n",
                  currentEvent.c_str(), AI_RESULT_WAIT_MS / 1000);
  }
  if (now - waitStarted >= AI_RESULT_WAIT_MS)
    return finish("AI wait expired: result unknown; no alarm. Check dashboard.");
  if (now - lastPoll < AI_POLL_INTERVAL_MS) return true;
  lastPoll = now;
  if (WiFi.status() != WL_CONNECTED) return true;

  String url = projectServerUrl("/api/device/event-status?device_id=");
  if (!url.length())
    return finish("AI alarm disabled: use an HTTPS upload URL ending in /api/device/upload.");
  url += encodeQuery(String(DEVICE_ID));
  url += "&event_id=" + encodeQuery(currentEvent);
  url += "&expected=" + String(expectedPhotos);

  WiFiClientSecure client;
  if (!configureProjectTls(client)) return true;
  client.setHandshakeTimeout(5);
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(3000);
  if (!http.begin(client, url)) return true;
  addProjectDeviceHeaders(http);
  http.addHeader("X-Demo-Alarm", "1");
  http.addHeader("Cache-Control", "no-cache");
  const char *headers[] = {"Transfer-Encoding"};
  http.collectHeaders(headers, 1);
  int code = http.GET();
  logProjectTransportError(client, code);
  String status = code == 200 ? readStatusWord(http, client) : String();
  http.end();
  Serial.printf("AI status: HTTP %d %s\n", code, status.c_str());
  if (code == 401 || code == 403)
    return finish("AI authentication rejected: check DEVICE_UPLOAD_TOKEN.");
  if (code != 200) return true;
  if (millis() - waitStarted >= AI_RESULT_WAIT_MS)
    return finish("AI wait expired during request: no alarm.");
  if (status == "suspicious" || status == "demo_alarm") {
    sounding = status == "demo_alarm" ? Alarm::Demo : Alarm::Suspicious;
    if (!startPulsing()) return finish("Alarm output timer failed; check the board.");
    alarmStarted = millis();
    Serial.printf("%s; red LED and buzzer pulse every %lu ms for %lu seconds.\n",
                  sounding == Alarm::Demo ? "DEMO ALARM: dashboard test" : "AI ALARM: current event suspicious",
                  ALARM_PULSE_MS, ALARM_DURATION_MS / 1000UL);
    return true;
  }
  if (status == "normal") return finish("AI result normal: no alarm.");
  // Only a server decision can suppress an alarm through the family policy.
  if (status == "exempted") return finish("AI result exempted by server family policy: no alarm. See recognition history.");
  if (status == "review") return finish("AI result needs manual review: no alarm.");
  // pending, not_found, invalid response or unknown status: wait within the limit.
  return true;
}
