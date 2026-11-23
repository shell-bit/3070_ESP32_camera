#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include "project_sensors.h"
#include "project_settings.h"
#include "project_sleep.h"
#include "project_transport.h"
#include "project_audio.h"

// The S3 ROM audio compressor needs temporary stack space in addition to its PSRAM buffers.
SET_LOOP_TASK_STACK_SIZE(24 * 1024);

// Same ESP32-S3 camera connector for OV3660 and OV5640 modules.
#include "camera_pins.h"

#if __has_include("wifi_settings.h")
#include "wifi_settings.h"
#else
#include "wifi_settings.example.h"
#endif

// A hint for the first channel to scan on a PIR wake. Roaming stays enabled.
RTC_DATA_ATTR uint8_t lastWifiChannel = 0;

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();
  Serial.printf("Camera: %s\n", DEVICE_ID);
  prepareProjectWake();

  if (!psramFound()) {
    Serial.println("Photo bursts require PSRAM. Enable OPI PSRAM in board settings.");
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
  config.xclk_freq_hz = OV3660_FULL_HD ? 20000000 : 16000000;
  config.frame_size = OV3660_FULL_HD ? FRAMESIZE_FHD : FRAMESIZE_VGA;
  config.pixel_format = OV3660_FULL_HD ? PIXFORMAT_JPEG : PIXFORMAT_RGB565;
  // One frame at a time reduces camera/PSRAM traffic while audio is recorded.
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = PHOTO_JPEG_QUALITY;
  config.fb_count = 1;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera initialization failed: 0x%x\n", err);
    sleepProjectUntilMotion();
    return;
  }

  sensor_t *sensor = esp_camera_sensor_get();
  if (!sensor) {
    Serial.println("Camera sensor unavailable after initialization.");
    sleepProjectUntilMotion();
    return;
  }
  if (sensor->id.PID != OV3660_PID && config.pixel_format != PIXFORMAT_JPEG) {
    // Pixel format changes the DMA layout. Reinitialize the driver for the
    // original Full HD JPEG camera and reacquire its sensor object.
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size = FRAMESIZE_FHD;
    config.xclk_freq_hz = 20000000;
    err = esp_camera_reconfigure(&config);
    sensor = esp_camera_sensor_get();
    if (err != ESP_OK || !sensor) {
      Serial.printf("Could not configure native JPEG capture: 0x%x\n", err);
      sleepProjectUntilMotion();
      return;
    }
  }
  sensor->set_vflip(sensor, 1);
  const char *sensorName = "Other supported sensor";
  if (sensor->id.PID == OV3660_PID) {
    sensorName = "OV3660";
    if (config.pixel_format == PIXFORMAT_JPEG &&
        (sensor->set_xclk(sensor, config.ledc_timer, 16) != 0 ||
         sensor->set_framesize(sensor, config.frame_size) != 0 ||
         sensor->set_quality(sensor, OV3660_JPEG_QUALITY) != 0)) {
      Serial.println("OV3660 rejected capture settings.");
      sleepProjectUntilMotion();
      return;
    }
    int settingsResult = sensor->set_exposure_ctrl(sensor, 1);
    settingsResult |= sensor->set_gain_ctrl(sensor, 1);
    settingsResult |= sensor->set_whitebal(sensor, 1);
    settingsResult |= sensor->set_wb_mode(sensor, 0);
    settingsResult |= sensor->set_brightness(sensor, 0);
    settingsResult |= sensor->set_saturation(sensor, 0);
    if (settingsResult != 0)
      Serial.println("OV3660 image settings were not fully applied; check the captured image.");
    else
      Serial.println("OV3660 image settings: auto exposure and white balance, neutral brightness and color.");
  } else if (sensor->id.PID == OV5640_PID) {
    sensorName = "OV5640";
  }
  // The raw-register noise profile below is only for the original OV5640.
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
  const resolution_info_t &size = resolution[sensor->status.framesize];
  bool softwareJpeg = config.pixel_format == PIXFORMAT_RGB565;
  Serial.printf("Sensor: %s (PID 0x%04x). Photos: %ux%u. %s, quality %d. XCLK: %d MHz.\n",
                sensorName, sensor->id.PID, unsigned(size.width), unsigned(size.height),
                softwareJpeg ? "RGB565 -> software JPEG" : "Native JPEG",
                softwareJpeg ? int(OV3660_SOFTWARE_JPEG_QUALITY) : int(sensor->status.quality),
                sensor->xclk_freq_hz / 1000000);

  setupProjectSensors();
  captureProjectWakeBurst();
  powerDownProjectCamera();

  unsigned long wifiStart = millis();
  WiFi.persistent(false);
  WiFi.begin(ssid, password, lastWifiChannel);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 20000) {
    delay(50);
  }
  Serial.println();
  Serial.println(WiFi.status() == WL_CONNECTED
    ? "WiFi connected"
    : "WiFi not connected yet; will reconnect automatically.");

  if (WiFi.status() == WL_CONNECTED) {
    lastWifiChannel = WiFi.channel();
    Serial.printf("ESP32 local IP: %s; Wi-Fi connected in %lu ms on channel %u.\n",
                  WiFi.localIP().toString().c_str(), millis() - wifiStart, unsigned(lastWifiChannel));
  }
  syncProjectClock();
  // The camera is powered down. The microphone can finish its clip while the
  // network sends the already captured pictures to the worker.
  Serial.println("Uploading event, checking AI alarm, then listening for dashboard speech.");
}

void loop() {
  pollProjectSensors();
  delay(20);
}
