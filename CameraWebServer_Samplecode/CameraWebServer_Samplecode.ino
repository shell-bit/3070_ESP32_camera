#include "esp_camera.h"
#include <WiFi.h>
#include "project_sensors.h"
#include "project_settings.h"
#include "project_sleep.h"
#include "project_transport.h"
#include "project_audio.h"

// ESP32-S3 / OV5640 wiring used by this project.
#include "camera_pins.h"

#if __has_include("wifi_settings.h")
#include "wifi_settings.h"
#else
#include "wifi_settings.example.h"
#endif

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();
  Serial.printf("Camera: %s\n", DEVICE_ID);
  prepareProjectWake();

  if (!psramFound()) {
    Serial.println("Full HD photo bursts require PSRAM. Enable OPI PSRAM in board settings.");
    sleepProjectUntilMotion();
    return;
  }

  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size = FRAMESIZE_FHD;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = PHOTO_JPEG_QUALITY;
  config.fb_count = 2;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera initialization failed: 0x%x\n", err);
    sleepProjectUntilMotion();
    return;
  }

  sensor_t *sensor = esp_camera_sensor_get();
  if (!sensor || sensor->set_framesize(sensor, FRAMESIZE_FHD) != 0) {
    Serial.println("Camera rejected Full HD resolution.");
    sleepProjectUntilMotion();
    return;
  }
  sensor->set_vflip(sensor, 1);
  if (sensor->id.PID == OV5640_PID && CAMERA_NOISE_REDUCTION) {
    int settingsResult = sensor->set_exposure_ctrl(sensor, 1);
    settingsResult |= sensor->set_gain_ctrl(sensor, 1);
    settingsResult |= sensor->set_whitebal(sensor, 1);
    // OV5640 gain ceiling is a 10-bit value in units of 1/16x.
    // Its driver does not map the generic GAINCEILING_* enum to that value.
    const int gainLimit = CAMERA_MAX_GAIN_X * 16;
    settingsResult |= sensor->set_reg(sensor, 0x3A18, 0x03, gainLimit >> 8);
    settingsResult |= sensor->set_reg(sensor, 0x3A19, 0xFF, gainLimit & 0xFF);
    settingsResult |= sensor->set_sharpness(sensor, -1);
    settingsResult |= sensor->set_denoise(sensor, 3);
    if (settingsResult != 0 || sensor->get_reg(sensor, 0x3A18, 0x3FF) != gainLimit) {
      Serial.println("Camera noise settings were not fully applied; check sensor connection.");
    } else {
      Serial.printf("OV5640 noise tuning: auto exposure, gain limit %dx, mild denoise.\n", CAMERA_MAX_GAIN_X);
    }
  }
  Serial.printf("Sensor PID: 0x%04x. Photos: 1920x1080. JPEG quality: %d\n",
                sensor->id.PID, PHOTO_JPEG_QUALITY);

  setupProjectSensors();
  captureProjectWakeBurst();
  powerDownProjectCamera();

  WiFi.begin(ssid, password);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 20000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.println(WiFi.status() == WL_CONNECTED
    ? "WiFi connected"
    : "WiFi not connected yet; will reconnect automatically.");

  syncProjectClock();
  reportProjectAudioState("uploading");
  Serial.println("Uploading event, checking AI alarm, then listening for dashboard speech.");
}

void loop() {
  pollProjectSensors();
  delay(20);
}
