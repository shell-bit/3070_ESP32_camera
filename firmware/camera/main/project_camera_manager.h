#pragma once
#include <Arduino.h>
#include <memory>
#include "esp_camera.h"
enum class CameraMode { Idle, Preview, Event, Enrollment };
struct OwnedJpeg {
  std::shared_ptr<uint8_t> bytes;
  size_t size = 0;
  uint16_t width = 0, height = 0;
  explicit operator bool() const {
    return bytes && size;
  }
};
// All sensor/driver access lives in project_camera_manager.cpp. No raw fb escapes.
void setupCameraManager(const camera_config_t &config);
bool beginCameraOperation(CameraMode mode);
OwnedJpeg captureCameraJpeg(CameraMode owner, size_t limit);
void endCameraOperation(CameraMode owner);
void idleCameraPreview();
void shutdownCameraManager();
CameraMode currentCameraMode();
