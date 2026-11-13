#include "project_event_upload.h"
#include "project_settings.h"
#include "project_transport.h"
#include "cJSON.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <algorithm>
String projectApiUrl(const char *path) {
  String url = LOCAL_DASHBOARD_UPLOAD_URL;
  String suffix = "/api/device/upload";
  if (!url.endsWith(suffix))
    return {};
  url.remove(url.length() - suffix.length());
  return url + path;
}
namespace {
// Three immutable spans, streamed without allocating a second FHD JPEG.
class Multipart : public Stream {
  const uint8_t *parts[3];
  size_t sizes[3], pos = 0;
  int part = 0;

public:
  Multipart(const String &a, const OwnedJpeg &b, const String &c) {
    parts[0] = (const uint8_t *)a.c_str();
    sizes[0] = a.length();
    parts[1] = b.bytes.get();
    sizes[1] = b.size;
    parts[2] = (const uint8_t *)c.c_str();
    sizes[2] = c.length();
  }
  int available() override {
    size_t n = 0;
    for (int i = part; i < 3; ++i)
      n += sizes[i] - (i == part ? pos : 0);
    return int(n);
  }
  int read() override {
    while (part < 3 && pos == sizes[part]) {
      ++part;
      pos = 0;
    }
    return part < 3 ? parts[part][pos++] : -1;
  }
  int peek() override {
    int p = part;
    size_t i = pos;
    while (p < 3 && i == sizes[p]) {
      ++p;
      i = 0;
    }
    return p < 3 ? parts[p][i] : -1;
  }
  size_t write(uint8_t) override {
    return 0;
  }
};
} // namespace
UploadResult uploadEventFrame(const OwnedJpeg &photo, const String &metadata, const String &event,
                              int index, bool night, unsigned long remaining) {
  if (remaining < 100)
    return UploadResult::Retry;
  WiFiClientSecure client;
  if (!configureProjectTls(client))
    return UploadResult::Retry;
  HTTPClient http;
  http.setConnectTimeout(std::min(remaining, UPLOAD_CONNECT_TIMEOUT_MS));
  http.setTimeout(std::min(remaining, UPLOAD_RESPONSE_TIMEOUT_MS));
  String url = PROJECT_UPLOAD_V2 ? projectApiUrl("/api/device/upload-v2")
                                 : String(LOCAL_DASHBOARD_UPLOAD_URL);
  if (!url.length() || !http.begin(client, url))
    return UploadResult::Rejected;
  http.addHeader("Authorization", String("Bearer ") + DEVICE_UPLOAD_TOKEN);
  http.addHeader("ngrok-skip-browser-warning", "1");
  http.addHeader("X-Device-ID", DEVICE_ID);
  http.addHeader("X-Event-ID", event);
  http.addHeader("X-Photo-Index", String(index));
  http.addHeader("X-Expected-Photos", "5");
  http.addHeader("X-Night-Capture", night ? "true" : "false");
  int code;
  if (PROJECT_UPLOAD_V2) {
    if (metadata.isEmpty() || metadata.length() > 16384) {
      http.end();
      return UploadResult::Rejected;
    }
    String head = "--camera-event\r\nContent-Disposition: form-data; "
                  "name=\"metadata\"\r\nContent-Type: application/json\r\n\r\n" +
                  metadata +
                  "\r\n--camera-event\r\nContent-Disposition: form-data; name=\"photo\"; "
                  "filename=\"frame.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n";
    String tail = "\r\n--camera-event--\r\n";
    Multipart body(head, photo, tail);
    http.addHeader("Content-Type", "multipart/form-data; boundary=camera-event");
    code = http.sendRequest("POST", &body, head.length() + photo.size + tail.length());
  } else {
    http.addHeader("Content-Type", "image/jpeg");
    code = http.POST(photo.bytes.get(), photo.size);
  }
  bool saved = false;
  if (code == 201 && http.getSize() >= 0 && http.getSize() <= 4096) {
    auto *response = cJSON_Parse(http.getString().c_str());
    saved = response && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(response, "saved"));
    cJSON_Delete(response);
  }
  http.end();
  Serial.printf("Frame %d HTTP=%d saved=%d\n", index, code, saved);
  if (saved)
    return UploadResult::Saved;
  if (code == 400 || code == 401 || code == 403 || code == 409 || code == 413 || code == 415 ||
      code == 422)
    return UploadResult::Rejected;
  return UploadResult::Retry;
}
