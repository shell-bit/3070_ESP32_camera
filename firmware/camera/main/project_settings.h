#pragma once
#include "project_config.h"

// GPIO 48 controls a suitable driver/MOSFET, never a 3 W LED directly.
static const int RED_LED_PIN = 48;
static const int RED_LED_ACTIVE_LEVEL = HIGH;
static const bool AI_ALARM_ENABLED = true;
static const unsigned long AI_RESULT_WAIT_MS = 60000;
static const unsigned long AI_POLL_INTERVAL_MS = 3000;
// Match the server UPLOAD_TOKEN if enabled; otherwise leave empty.
static const char DEVICE_UPLOAD_TOKEN[] = PROJECT_DEVICE_TOKEN;

// PIR wake -> capture -> bounded upload -> bounded AI result/alarm -> deep sleep.
static const bool LOW_POWER_MODE = PROJECT_DEEP_SLEEP;

// Sensor and output pins
static const int PIR_PIN = 1;
static const int LIGHT_SENSOR_PIN = 2;
static const int WHITE_LED_PIN = 21;
static const int BUZZER_PIN = 47;
static const int BUZZER_ACTIVE_LEVEL = HIGH;
static const int TRIGGER_BUTTON_PIN = -1;
static const unsigned long BUZZER_DURATION_MS = 3000;

static const int PIR_ACTIVE_LEVEL = HIGH;
static const int WHITE_LED_ACTIVE_LEVEL = HIGH;

// Calibrate using readings taken in bright and dark conditions.
static const bool DARK_WHEN_READING_LOW = false;
static const int DARK_THRESHOLD = 1500;

static const unsigned long PIR_WARMUP_MS = 30000;
static const unsigned long TRIGGER_COOLDOWN_MS = 10000;
static const unsigned long PHOTO_INTERVAL_MS = 500;
static const unsigned long LIGHT_SETTLE_MS = 700;

// Cloud upload timing
static const unsigned long UPLOAD_RETRY_MS = 2000;
static const unsigned long UPLOAD_GIVE_UP_MS = 60000;
static const unsigned long UPLOAD_CONNECT_TIMEOUT_MS = 5000;
static const unsigned long UPLOAD_RESPONSE_TIMEOUT_MS = 15000;

static const int BURST_COUNT = 5;

// Smaller JPEGs than quality 12; resolution is configured elsewhere.
static const int PHOTO_JPEG_QUALITY = 16;

// Public cloud upload endpoint.
// Keep this variable name because your existing upload code uses it.
static const char LOCAL_DASHBOARD_UPLOAD_URL[] = PROJECT_UPLOAD_URL;

static const char DEVICE_ID[] = PROJECT_DEVICE_ID;

// Audio: separate clocks for INMP441 and MAX98357; ESP32 Arduino core 3.3.12.
static const bool EVENT_AUDIO_ENABLED = true;
static const unsigned int EVENT_AUDIO_SECONDS = 4; // Choose 3, 4 or 5.
static_assert(EVENT_AUDIO_SECONDS >= 3 && EVENT_AUDIO_SECONDS <= 5, "Use 3 to 5 seconds");
static const int MIC_BCLK_PIN = 39;
static const int MIC_WS_PIN = 40;
static const int MIC_DATA_PIN = 41; // INMP441 L/R -> GND, VDD -> 3.3V.
static const int PROJECT_MIC_GAIN = 4;
static const int SPEAKER_BCLK_PIN = 14;
static const int SPEAKER_WS_PIN = 38;
static const int SPEAKER_DATA_PIN = 42; // MAX98357 SD -> 3.3V, VIN -> 5V.
static const float PROJECT_SPEAKER_VOLUME = 1.0f;
static const bool TALK_ENABLED = true;
static const unsigned long TALK_WINDOW_MS = 90000; // After uploads; bounded low-power wait.
