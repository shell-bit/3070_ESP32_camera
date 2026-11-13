#include "project_camera_manager.h"
#include "project_settings.h"
#include "esp_heap_caps.h"
#include "freertos/semphr.h"
namespace {
SemaphoreHandle_t lock = nullptr;
camera_config_t config{};
CameraMode mode = CameraMode::Idle;
bool initialized = false;
struct Guard {
  bool held;
  Guard() : held(lock && xSemaphoreTake(lock, pdMS_TO_TICKS(1000)) == pdTRUE) {}
  ~Guard() {
    if (held)
      xSemaphoreGive(lock);
  }
};
void stop() {
  if (initialized) {
    auto *s = esp_camera_sensor_get();
    if (s && s->id.PID == OV5640_PID)
      s->set_reg(s, 0x3008, 0x40, 0x40);
    esp_camera_deinit();
    initialized = false;
  }
  mode = CameraMode::Idle;
}
bool configure(CameraMode next) {
  if (!initialized) {
    if (!psramFound())
      return false;
    config.frame_size = FRAMESIZE_FHD;
    if (esp_camera_init(&config) != ESP_OK)
      return false;
    initialized = true;
  }
  auto *s = esp_camera_sensor_get();
  if (!s)
    return false;
  auto size = next == CameraMode::Event
                  ? FRAMESIZE_FHD
                  : (next == CameraMode::Enrollment ? FRAMESIZE_VGA : FRAMESIZE_QVGA);
  if (s->set_framesize(s, size) != 0 || s->set_quality(s, PHOTO_JPEG_QUALITY) != 0) {
    stop();
    return false;
  }
  s->set_vflip(s, 1);
  for (int i = 0; i < 2; ++i) {
    auto *fb = esp_camera_fb_get();
    if (fb)
      esp_camera_fb_return(fb);
  }
  mode = next;
  return true;
}
} // namespace
void setupCameraManager(const camera_config_t &c) {
  config = c;
  lock = xSemaphoreCreateMutex();
}
bool beginCameraOperation(CameraMode next) {
  Guard g;
  if (!g.held)
    return false;
  if (mode == next)
    return next == CameraMode::Preview;
  if (mode == CameraMode::Event)
    return false;
  if (next == CameraMode::Preview && mode == CameraMode::Enrollment)
    return false;
  return configure(next);
}
OwnedJpeg captureCameraJpeg(CameraMode owner, size_t limit) {
  Guard g;
  OwnedJpeg out;
  if (!g.held || mode != owner || !initialized)
    return out;
  auto *fb = esp_camera_fb_get();
  if (fb && fb->format == PIXFORMAT_JPEG && fb->len <= limit) {
    auto *p =
        static_cast<uint8_t *>(heap_caps_malloc(fb->len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (p) {
      memcpy(p, fb->buf, fb->len);
      out.bytes = std::shared_ptr<uint8_t>(p, free);
      out.size = fb->len;
      out.width = fb->width;
      out.height = fb->height;
    }
  }
  if (fb)
    esp_camera_fb_return(fb);
  return out;
}
void endCameraOperation(CameraMode owner) {
  Guard g;
  if (g.held && mode == owner)
    stop();
}
void idleCameraPreview() {
  endCameraOperation(CameraMode::Preview);
}
void shutdownCameraManager() {
  Guard g;
  if (g.held)
    stop();
}
CameraMode currentCameraMode() {
  Guard g;
  return g.held ? mode : CameraMode::Event;
}
