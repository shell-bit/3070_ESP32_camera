#include <Arduino.h>
#include <WiFi.h>
#include "esp_camera.h"
#include "project_settings.h"
#include "project_sensors.h"
#include "project_sleep.h"
#include "project_transport.h"
#include "project_camera_manager.h"
#include "project_preview.h"
#include "project_face_identity.h"
#include "project_device_commands.h"
#define CAMERA_MODEL_ESP32S3_EYE
#include "camera_pins.h"
void setup() {
  Serial.begin(115200);
  prepareProjectWake();
  camera_config_t c{};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM;
  c.pin_d1 = Y3_GPIO_NUM;
  c.pin_d2 = Y4_GPIO_NUM;
  c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM;
  c.pin_d5 = Y7_GPIO_NUM;
  c.pin_d6 = Y8_GPIO_NUM;
  c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM;
  c.pin_pclk = PCLK_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM;
  c.pin_href = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM;
  c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM;
  c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000;
  c.frame_size = FRAMESIZE_FHD;
  c.pixel_format = PIXFORMAT_JPEG;
  c.fb_count = 2;
  c.jpeg_quality = PHOTO_JPEG_QUALITY;
  c.fb_location = CAMERA_FB_IN_PSRAM;
  c.grab_mode = CAMERA_GRAB_LATEST;
  setupCameraManager(c);
  setupProjectSensors();
  setupFaceIdentity();
  if (LOW_POWER_MODE)
    captureProjectWakeBurst();
  WiFi.mode(WIFI_STA);
  WiFi.begin(PROJECT_WIFI_SSID, PROJECT_WIFI_PASSWORD);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(true);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000)
    delay(100);
  syncProjectClock();
  if (!LOW_POWER_MODE)
    startPreviewServer();
  Serial.printf("Camera online mode=%s PSRAM=%u free=%u, control=80 stream=81\n",
                LOW_POWER_MODE ? "deep-sleep" : "wifi-idle", ESP.getPsramSize(),
                ESP.getFreePsram());
}
void loop() {
  pollProjectSensors();
  if (!LOW_POWER_MODE)
    pollDeviceCommands();
  delay(10);
}
