#include "project_device_commands.h"
#include "project_face_identity.h"
#include "project_camera_manager.h"
#include "project_event_upload.h"
#include "project_settings.h"
#include "project_sensors.h"
#include "project_transport.h"
#include "esp_timer.h"
#include "cJSON.h"
#include <HTTPClient.h>
#include <WiFi.h>
#include <cmath>
#include <ctime>
#include <algorithm>
#include "freertos/queue.h"
namespace {
struct Job {
  String id, member;
  uint32_t version = 0;
  uint64_t deadline = 0;
  std::vector<std::vector<float>> samples;
  std::vector<float> poses;
  bool sampling = false;
};
Job job;
unsigned long lastSample = 0;
QueueHandle_t inbox = nullptr, outbox = nullptr;
struct Message {
  String url, body;
};
String text(cJSON *o, const char *key) {
  auto *v = cJSON_GetObjectItemCaseSensitive(o, key);
  return cJSON_IsString(v) ? String(v->valuestring) : String();
}
cJSON *request(const String &url, cJSON *body = nullptr, int *responseCode = nullptr) {
  if (responseCode)
    *responseCode = -1;
  if (url.isEmpty())
    return nullptr;
  WiFiClientSecure tls;
  if (!configureProjectTls(tls)) {
    cJSON_Delete(body);
    return nullptr;
  }
  HTTPClient http;
  http.setConnectTimeout(1500);
  http.setTimeout(2000);
  if (!http.begin(tls, url)) {
    cJSON_Delete(body);
    return nullptr;
  }
  http.addHeader("Authorization", String("Bearer ") + DEVICE_UPLOAD_TOKEN);
  http.addHeader("ngrok-skip-browser-warning", "1");
  int code;
  if (body) {
    char *p = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!p) {
      http.end();
      return nullptr;
    }
    http.addHeader("Content-Type", "application/json");
    code = http.POST((uint8_t *)p, strlen(p));
    free(p);
  } else
    code = http.GET();
  if (responseCode)
    *responseCode = code;
  cJSON *result = nullptr;
  if (code >= 200 && code < 300 && http.getSize() >= 0 && http.getSize() <= 16384)
    result = cJSON_Parse(http.getString().c_str());
  http.end();
  return result;
}
bool enqueue(const String &url, cJSON *body) {
  char *serialized = cJSON_PrintUnformatted(body);
  cJSON_Delete(body);
  if (!serialized)
    return false;
  auto *message = new Message{url, serialized};
  free(serialized);
  if (!outbox || xQueueSend(outbox, &message, 0) != pdTRUE) {
    delete message;
    return false;
  }
  return true;
}
// Cloud HTTP never blocks the main-loop PIR checks or owns camera/registry objects.
void commandNetwork(void *) {
  Message *pending = nullptr;
  unsigned long lastFetch = millis() - 3000;
  while (true) {
    if (WiFi.status() != WL_CONNECTED) {
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }
    if (!pending)
      xQueueReceive(outbox, &pending, 0);
    if (pending) {
      int code = -1;
      auto *result = request(pending->url, cJSON_Parse(pending->body.c_str()), &code);
      cJSON_Delete(result);
      if ((code >= 200 && code < 300) || (code >= 400 && code < 500 && code != 429)) {
        delete pending;
        pending = nullptr;
      } else {
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }
    }
    if (pending || uxQueueMessagesWaiting(outbox)) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    if (millis() - lastFetch >= 3000 && uxQueueMessagesWaiting(inbox) == 0) {
      lastFetch = millis();
      String path = "/api/device/commands?device_id=" + String(DEVICE_ID);
      auto *result = request(projectApiUrl(path.c_str()));
      if (result && xQueueSend(inbox, &result, 0) != pdTRUE)
        cJSON_Delete(result);
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
bool ack(const Job &j, const char *result, const char *error = nullptr) {
  auto *b = cJSON_CreateObject();
  cJSON_AddStringToObject(b, "device_id", DEVICE_ID);
  cJSON_AddNumberToObject(b, "member_version", j.version);
  cJSON_AddStringToObject(b, "result", result);
  if (!strcmp(result, "committed")) {
    uint32_t databaseVersion = faceStore().registry.version;
    for (const auto &t : faceStore().registry.entries)
      if (t.command == j.id.c_str())
        databaseVersion = t.databaseVersion;
    cJSON_AddStringToObject(b, "database_version", String(databaseVersion).c_str());
    cJSON_AddStringToObject(b, "recognizer_version", faceRecognizerVersion());
  }
  if (error)
    cJSON_AddStringToObject(b, "error", error);
  String path = "/api/device/commands/" + j.id + "/ack";
  return enqueue(projectApiUrl(path.c_str()), b);
}
bool alreadyCommitted(const Job &j) {
  for (const auto &t : faceStore().registry.entries)
    if (t.member == j.member.c_str() && t.command == j.id.c_str() && t.version == j.version)
      return true;
  return false;
}
void clear() {
  endCameraOperation(CameraMode::Enrollment);
  job = {};
}
void sample() {
  if (job.id.isEmpty())
    return;
  uint64_t now = esp_timer_get_time() / 1000;
  if (now >= job.deadline) {
    ack(job, "failed", "Enrollment expired before durable commit");
    clear();
    return;
  }
  if (projectEventBusy() || currentCameraMode() == CameraMode::Event) {
    job.samples.clear();
    job.poses.clear();
    job.sampling = false;
    return;
  }
  if (currentCameraMode() != CameraMode::Enrollment) {
    job.samples.clear();
    job.poses.clear();
    job.sampling = false;
    if (!beginCameraOperation(CameraMode::Enrollment))
      return;
  }
  if (millis() - lastSample < 700)
    return;
  lastSample = millis();
  auto jpeg = captureCameraJpeg(CameraMode::Enrollment, 256 * 1024);
  if (!jpeg)
    return;
  std::vector<float> feature;
  float pose = 0;
  if (!extractEnrollmentFeature(jpeg, feature, pose))
    return;
  if (uint64_t(esp_timer_get_time() / 1000) >= job.deadline)
    return;
  if (!camera::consistentSample(job.samples, feature, PROJECT_ENROLLMENT_MIN_SIMILARITY)) {
    Serial.println("Enrollment sample rejected: identity differs from earlier samples.");
    return;
  }
  for (float old : job.poses)
    if (std::abs(old - pose) < 0.08f)
      return;
  job.samples.push_back(std::move(feature));
  job.poses.push_back(pose);
  if (job.samples.size() < 3)
    return;
  auto next = faceStore().registry;
  if (!next.commit(job.member.c_str(), job.version, job.id.c_str(), job.samples,
                   PROJECT_ENROLLMENT_MIN_SIMILARITY) ||
      !faceStore().save(next)) {
    ack(job, "failed", "Feature database commit failed or capacity reached");
    clear();
    return;
  }
  ack(job, "committed");
  clear();
}
} // namespace
void pollDeviceCommands() {
  if (!PROJECT_FACE_ENABLED || !PROJECT_UPLOAD_V2 || !DEVICE_UPLOAD_TOKEN[0] ||
      !LOCAL_DASHBOARD_UPLOAD_URL[0])
    return;
  if (!inbox) {
    inbox = xQueueCreate(1, sizeof(cJSON *));
    outbox = xQueueCreate(32, sizeof(Message *));
    if (!inbox || !outbox) {
      if (inbox)
        vQueueDelete(inbox);
      if (outbox)
        vQueueDelete(outbox);
      inbox = outbox = nullptr;
      return;
    }
    std::vector<const camera::Template *> receipts;
    for (const auto &t : faceStore().registry.entries)
      receipts.push_back(&t);
    std::sort(receipts.begin(), receipts.end(),
              [](auto *a, auto *b) { return a->databaseVersion < b->databaseVersion; });
    // Replay durable receipts BEFORE reporting a possibly newer library version.
    for (const auto *t : receipts) {
      Job committed;
      committed.id = t->command.c_str();
      committed.member = t->member.c_str();
      committed.version = t->version;
      ack(committed, "committed");
    }
    auto *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "device_id", DEVICE_ID);
    cJSON_AddStringToObject(b, "recognizer_version", faceRecognizerVersion());
    cJSON_AddStringToObject(b, "database_version", faceDatabaseVersion().c_str());
    cJSON_AddBoolToObject(b, "compatible", faceIdentityReady());
    cJSON_AddBoolToObject(b, "reset", !faceStore().ready());
    enqueue(projectApiUrl("/api/device/identity-state"), b);
    if (xTaskCreate(commandNetwork, "device-commands", 8192, nullptr, 1, nullptr) != pdPASS) {
      Message *m;
      while (xQueueReceive(outbox, &m, 0) == pdTRUE)
        delete m;
      vQueueDelete(inbox);
      vQueueDelete(outbox);
      inbox = outbox = nullptr;
    }
    return;
  }
  cJSON *root = nullptr;
  if (xQueueReceive(inbox, &root, 0) != pdTRUE) {
    sample();
    return;
  }
  auto *commands = cJSON_GetObjectItemCaseSensitive(root, "commands");
  if (!cJSON_IsArray(commands)) {
    cJSON_Delete(root);
    return;
  }
  time_t utc = time(nullptr);
  if (utc < 1735689600) {
    cJSON_Delete(root);
    return;
  }
  bool activeStillPresent = job.id.isEmpty();
  cJSON *item = nullptr;
  cJSON_ArrayForEach(item, commands) {
    Job incoming;
    incoming.id = text(item, "id");
    incoming.member = text(item, "member_id");
    auto *v = cJSON_GetObjectItemCaseSensitive(item, "member_version");
    auto *expiry = cJSON_GetObjectItemCaseSensitive(item, "expires_at");
    String op = text(item, "operation");
    if (text(item, "device_id") != DEVICE_ID || !camera::validIdentifier(incoming.id.c_str()) ||
        !camera::validIdentifier(incoming.member.c_str()) || !cJSON_IsNumber(v) ||
        v->valuedouble < 1 || v->valuedouble > UINT32_MAX ||
        floor(v->valuedouble) != v->valuedouble || !cJSON_IsNumber(expiry) ||
        expiry->valuedouble <= double(utc))
      continue;
    incoming.version = uint32_t(v->valuedouble);
    incoming.deadline = uint64_t(esp_timer_get_time() / 1000) +
                        uint64_t(std::min(120.0, expiry->valuedouble - double(utc)) * 1000);
    if (alreadyCommitted(incoming)) {
      ack(incoming, "committed");
      continue;
    }
    if (op == "revoke") {
      if (job.member == incoming.member)
        clear();
      auto next = faceStore().registry;
      if (next.revoke(incoming.member.c_str(), incoming.version, incoming.id.c_str()) &&
          faceStore().save(next))
        ack(incoming, "committed");
      else
        ack(incoming, "failed", "Revocation persistence failed");
      continue;
    }
    if (op != "enroll")
      continue;
    if (incoming.id == job.id) {
      activeStillPresent = true;
      continue;
    }
    if (!faceIdentityReady()) {
      ack(incoming, "failed", "Recognition model or dedicated face storage unavailable");
      continue;
    }
    if (!job.id.isEmpty())
      continue;
    job = std::move(incoming);
    activeStillPresent = true;
    ack(job, "sampling");
  }
  if (!activeStillPresent)
    clear();
  cJSON_Delete(root);
  sample();
}
