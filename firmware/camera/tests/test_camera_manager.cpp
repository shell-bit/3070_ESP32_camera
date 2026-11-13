#include "../main/project_camera_manager.h"
#include "../main/project_core.h"
#include "test_check.h"
#define assert CHECK
#include <iostream>
bool fakePsram = true;
bool fakeBlockLock = false;
static bool initialized = false, leased = false;
static int initializations = 0, returns = 0;
static framesize_t resolution = FRAMESIZE_FHD;
static uint8_t driverBytes[4];
static camera_fb_t driverFrame{driverBytes, 4, 320, 240, PIXFORMAT_JPEG};
static sensor_t sensor{{OV5640_PID},
                       [](sensor_t *, int, int, int) { return 0; },
                       [](sensor_t *, framesize_t size) {
                         resolution = size;
                         return 0;
                       },
                       [](sensor_t *, int) { return 0; },
                       [](sensor_t *, int) { return 0; }};
int esp_camera_init(camera_config_t *) {
  assert(!initialized);
  initialized = true;
  ++initializations;
  return 0;
}
int esp_camera_deinit() {
  assert(initialized && !leased);
  initialized = false;
  return 0;
}
sensor_t *esp_camera_sensor_get() {
  assert(initialized);
  return &sensor;
}
camera_fb_t *esp_camera_fb_get() {
  assert(initialized && !leased);
  leased = true;
  memset(driverBytes, 0xab, 4);
  return &driverFrame;
}
void esp_camera_fb_return(camera_fb_t *frame) {
  assert(leased && frame == &driverFrame);
  leased = false;
  ++returns;
  memset(driverBytes, 0xdd, 4);
}
int main() {
  camera_config_t config{};
  setupCameraManager(config);
  assert(beginCameraOperation(CameraMode::Preview) && resolution == FRAMESIZE_QVGA);
  auto owned = captureCameraJpeg(CameraMode::Preview, 100);
  assert(owned && owned.bytes.get()[0] == 0xab && !leased && returns == 3);
  assert(!captureCameraJpeg(CameraMode::Preview, 2) && !leased);
  camera::PendingTrigger trigger;
  trigger.request(100);
  fakeBlockLock = true;
  assert(!beginCameraOperation(CameraMode::Event));
  assert(trigger.pending && !trigger.expired(1000));
  fakeBlockLock = false;
  assert(beginCameraOperation(CameraMode::Event) && resolution == FRAMESIZE_FHD);
  trigger.consumed();
  assert(!trigger.pending && !beginCameraOperation(CameraMode::Event));
  assert(!captureCameraJpeg(CameraMode::Preview, 100));
  assert(!beginCameraOperation(CameraMode::Enrollment));
  idleCameraPreview();
  assert(currentCameraMode() == CameraMode::Event && initialized);
  endCameraOperation(CameraMode::Event);
  assert(!initialized);
  assert(beginCameraOperation(CameraMode::Enrollment) && resolution == FRAMESIZE_VGA);
  assert(!beginCameraOperation(CameraMode::Preview));
  assert(beginCameraOperation(CameraMode::Event));
  endCameraOperation(CameraMode::Enrollment);
  assert(initialized && currentCameraMode() == CameraMode::Event);
  endCameraOperation(CameraMode::Event);
  assert(!initialized && owned.bytes.get()[0] == 0xab);
  fakePsram = false;
  assert(!beginCameraOperation(CameraMode::Preview) && initializations == 2);
  std::cout
      << "camera manager: priority, stale release, private buffer and no-PSRAM checks passed\n";
}
