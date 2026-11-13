#include "project_preview.h"
#include "project_camera_manager.h"
#include "project_core.h"
#include "project_settings.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "cJSON.h"
#include <WiFi.h>
#include <atomic>
#include <mutex>
namespace {
std::mutex mu;
camera::PreviewSession session;
OwnedJpeg latest;
uint64_t sequence = 0;
std::atomic<bool> identityBusy{false};
httpd_handle_t control = nullptr, streamServer = nullptr;
uint64_t now() {
  return esp_timer_get_time() / 1000;
}
std::string state() {
  auto mode = currentCameraMode();
  if (mode == CameraMode::Event)
    return "paused_for_event";
  if (mode == CameraMode::Enrollment)
    return "paused_for_enrollment";
  if (identityBusy)
    return "paused_for_identity";
  return latest ? "streaming" : "starting";
}
esp_err_t error(httpd_req_t *r, int code, const char *message) {
  char status[48];
  snprintf(status, sizeof(status), "%d %s", code,
           code == 404   ? "Not Found"
           : code == 409 ? "Conflict"
           : code == 401 ? "Unauthorized"
                         : "Bad Request");
  httpd_resp_set_status(r, status);
  httpd_resp_set_type(r, "application/json");
  return httpd_resp_send(r, message, HTTPD_RESP_USE_STRLEN);
}
bool auth(httpd_req_t *r) {
  size_t n = httpd_req_get_hdr_value_len(r, "Authorization");
  if (n > 256 || n == 0)
    return false;
  char value[257]{};
  httpd_req_get_hdr_value_str(r, "Authorization", value, sizeof(value));
  return camera::constantTimeEqual(value, std::string("Bearer ") + PROJECT_PREVIEW_TOKEN);
}
std::string queryId(httpd_req_t *r) {
  char query[256]{}, id[129]{};
  if (httpd_req_get_url_query_str(r, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "session_id", id, sizeof(id)) != ESP_OK)
    return {};
  return id;
}
std::string pathId(httpd_req_t *r) {
  std::string path = r->uri, prefix = "/api/preview/session/";
  if (path.rfind(prefix, 0) != 0)
    return {};
  auto id = path.substr(prefix.size());
  auto p = id.find('/');
  if (p != std::string::npos)
    id.resize(p);
  return camera::validIdentifier(id) ? id : std::string();
}
esp_err_t json(httpd_req_t *r, cJSON *object) {
  char *p = cJSON_PrintUnformatted(object);
  cJSON_Delete(object);
  if (!p)
    return ESP_FAIL;
  httpd_resp_set_type(r, "application/json");
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  auto result = httpd_resp_send(r, p, HTTPD_RESP_USE_STRLEN);
  free(p);
  return result;
}
esp_err_t create(httpd_req_t *r) {
  if (!auth(r))
    return error(r, 401, "{\"error\":\"unauthorized\"}");
  if (r->content_len < 1 || r->content_len > 512)
    return error(r, 400, "{\"error\":\"invalid_body\"}");
  char body[513]{};
  int received = 0;
  while (received < int(r->content_len)) {
    int n = httpd_req_recv(r, body + received, r->content_len - received);
    if (n <= 0)
      return ESP_FAIL;
    received += n;
  }
  auto *root = cJSON_Parse(body);
  auto *request = root ? cJSON_GetObjectItemCaseSensitive(root, "request_id") : nullptr;
  std::string rid = cJSON_IsString(request) ? request->valuestring : "";
  cJSON_Delete(root);
  char id[34];
  snprintf(id, sizeof(id), "%08lx%08lx%08lx%08lx", (unsigned long)esp_random(),
           (unsigned long)esp_random(), (unsigned long)esp_random(), (unsigned long)esp_random());
  std::lock_guard<std::mutex> guard(mu);
  bool wasValid = session.valid(now());
  int code = session.open(rid, id, now());
  if (code != 202)
    return error(r, code, "{\"error\":\"invalid_or_busy\"}");
  if (!wasValid)
    latest = {};
  auto *out = cJSON_CreateObject();
  cJSON_AddStringToObject(out, "session_id", session.id.c_str());
  cJSON_AddStringToObject(out, "state", state().c_str());
  cJSON_AddNumberToObject(out, "lease_ms", session.deadline - now());
  std::string url = "http://" + std::string(WiFi.localIP().toString().c_str()) +
                    ":81/stream?session_id=" + session.id;
  cJSON_AddStringToObject(out, "stream_url", url.c_str());
  httpd_resp_set_status(r, "202 Accepted");
  return json(r, out);
}
esp_err_t status(httpd_req_t *r) {
  if (!auth(r))
    return error(r, 401, "{\"error\":\"unauthorized\"}");
  auto id = queryId(r);
  std::lock_guard<std::mutex> guard(mu);
  if (id != session.id || !session.valid(now()))
    return error(r, 404, "{\"error\":\"unknown_session\"}");
  auto *out = cJSON_CreateObject();
  cJSON_AddStringToObject(out, "session_id", session.id.c_str());
  cJSON_AddStringToObject(out, "state", state().c_str());
  cJSON_AddNumberToObject(out, "sequence", double(sequence));
  cJSON_AddNumberToObject(out, "lease_ms", double(session.deadline - now()));
  return json(r, out);
}
esp_err_t renew(httpd_req_t *r) {
  if (!auth(r))
    return error(r, 401, "{\"error\":\"unauthorized\"}");
  std::string path = r->uri;
  if (path.size() < 6 || path.substr(path.size() - 6) != "/renew")
    return error(r, 404, "{}");
  std::lock_guard<std::mutex> guard(mu);
  if (!session.renew(pathId(r), now()))
    return error(r, 404, "{\"error\":\"unknown_session\"}");
  httpd_resp_set_status(r, "204 No Content");
  return httpd_resp_send(r, nullptr, 0);
}
esp_err_t close(httpd_req_t *r) {
  if (!auth(r))
    return error(r, 401, "{\"error\":\"unauthorized\"}");
  std::lock_guard<std::mutex> guard(mu);
  if (session.close(pathId(r)))
    latest = {};
  httpd_resp_set_status(r, "204 No Content");
  return httpd_resp_send(r, nullptr, 0);
}
esp_err_t stream(httpd_req_t *r) {
  if (!auth(r))
    return error(r, 401, "{\"error\":\"unauthorized\"}");
  auto id = queryId(r);
  uint64_t gen;
  {
    std::lock_guard<std::mutex> guard(mu);
    if (id != session.id || !session.valid(now()))
      return error(r, 404, "{\"error\":\"unknown_session\"}");
    gen = session.generation;
  }
  httpd_resp_set_type(r, "multipart/x-mixed-replace; boundary=frame");
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  uint64_t sent = 0;
  while (true) {
    OwnedJpeg frame;
    uint64_t seq = 0;
    {
      std::lock_guard<std::mutex> guard(mu);
      if (id != session.id || gen != session.generation || !session.valid(now()))
        break;
      if (sequence != sent) {
        frame = latest;
        seq = sequence;
      }
    }
    if (!frame) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    char hdr[160];
    int n = snprintf(
        hdr, sizeof(hdr),
        "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\nX-Sequence: %llu\r\n\r\n",
        unsigned(frame.size), (unsigned long long)seq);
    if (httpd_resp_send_chunk(r, hdr, n) != ESP_OK ||
        httpd_resp_send_chunk(r, reinterpret_cast<const char *>(frame.bytes.get()), frame.size) !=
            ESP_OK ||
        httpd_resp_send_chunk(r, "\r\n", 2) != ESP_OK)
      return ESP_FAIL;
    sent = seq;
  }
  return httpd_resp_send_chunk(r, nullptr, 0);
}
void producer(void *) {
  while (true) {
    bool active = false, canCapture = false;
    uint64_t gen = 0;
    {
      std::lock_guard<std::mutex> guard(mu);
      active = session.valid(now());
      gen = session.generation;
      canCapture = !latest.bytes || latest.bytes.use_count() == 1;
      if (!active)
        latest = {};
    }
    if (!active || identityBusy) {
      idleCameraPreview();
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    if (canCapture && beginCameraOperation(CameraMode::Preview)) {
      auto frame = captureCameraJpeg(CameraMode::Preview, 128 * 1024);
      std::lock_guard<std::mutex> guard(mu);
      // A PIR operation may have started after this private frame was captured.
      if (frame && !identityBusy && currentCameraMode() == CameraMode::Preview &&
          session.valid(now()) && gen == session.generation) {
        latest = std::move(frame);
        ++sequence;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}
void route(httpd_handle_t h, const char *uri, httpd_method_t method,
           esp_err_t (*fn)(httpd_req_t *)) {
  httpd_uri_t r{};
  r.uri = uri;
  r.method = method;
  r.handler = fn;
  ESP_ERROR_CHECK(httpd_register_uri_handler(h, &r));
}
} // namespace
void setPreviewIdentityBusy(bool busy) {
  identityBusy = busy;
  if (busy) {
    std::lock_guard<std::mutex> guard(mu);
    latest = {};
  }
}
void startPreviewServer() {
  if (strlen(PROJECT_PREVIEW_TOKEN) < 24) {
    Serial.println("Preview disabled: configure a pairing token of at least 24 characters.");
    return;
  }
  httpd_config_t c = HTTPD_DEFAULT_CONFIG();
  c.server_port = 80;
  c.ctrl_port = 32768;
  c.max_uri_handlers = 8;
  c.uri_match_fn = httpd_uri_match_wildcard;
  c.stack_size = 8192;
  c.send_wait_timeout = 2;
  c.recv_wait_timeout = 2;
  if (httpd_start(&control, &c) != ESP_OK)
    return;
  route(control, "/api/preview/session", HTTP_POST, create);
  route(control, "/api/preview/status", HTTP_GET, status);
  route(control, "/api/preview/session/*", HTTP_POST, renew);
  route(control, "/api/preview/session/*", HTTP_DELETE, close);
  c.server_port = 81;
  c.ctrl_port = 32769;
  c.max_open_sockets = 1;
  if (httpd_start(&streamServer, &c) != ESP_OK) {
    httpd_stop(control);
    control = nullptr;
    return;
  }
  route(streamServer, "/stream", HTTP_GET, stream);
  xTaskCreate(producer, "preview", 6144, nullptr, 1, nullptr);
}
