#pragma once

// GPIO 48 controls a suitable driver/MOSFET, never a 3 W LED directly.
static const int RED_LED_PIN = 48;
static const int RED_LED_ACTIVE_LEVEL = HIGH;
static const bool AI_ALARM_ENABLED = true;
static const unsigned long AI_RESULT_WAIT_MS = 60000;
static const unsigned long AI_POLL_INTERVAL_MS = 3000;
// Required by the server for this camera's uploads, audio and alarm requests.
static const char DEVICE_UPLOAD_TOKEN[] = "6BniIJgPhjsmfBVKts6WyOW0d_G5D8TKBF7qjL3R2yk";

// Sensor and output pins
static const int PIR_PIN = 1;
static const int LIGHT_SENSOR_PIN = 2;
static const int WHITE_LED_PIN = 21;
static const int BUZZER_PIN = 47;
static const int BUZZER_ACTIVE_LEVEL = HIGH;
static constexpr unsigned long ALARM_DURATION_MS = 30000UL;
static constexpr unsigned long ALARM_PULSE_MS = 500UL; // Each on or off phase.

static const int PIR_ACTIVE_LEVEL = HIGH;
static const int WHITE_LED_ACTIVE_LEVEL = HIGH;

// Calibrate using readings taken in bright and dark conditions.
static const bool DARK_WHEN_READING_LOW = false;
static const int DARK_THRESHOLD = 1500;

static const unsigned long PIR_WARMUP_MS = 30000;
static const unsigned long PHOTO_INTERVAL_MS = 500;
static const unsigned long LIGHT_SETTLE_MS = 1200;
static const int CAMERA_WARMUP_FRAMES = 6;

// OV5640 tuning. Lower gain reduces noise but needs more light in dark scenes.
static const bool CAMERA_NOISE_REDUCTION = true;
static const int CAMERA_MAX_GAIN_X = 8;
static_assert(CAMERA_MAX_GAIN_X >= 1 && CAMERA_MAX_GAIN_X <= 32, "Use a gain limit from 1x to 32x");

// Cloud upload timing
static const unsigned long UPLOAD_RETRY_MS = 2000;
static const unsigned long UPLOAD_GIVE_UP_MS = 60000;
static const unsigned long UPLOAD_CONNECT_TIMEOUT_MS = 10000;
static const unsigned long UPLOAD_RESPONSE_TIMEOUT_MS = 15000;

static const int BURST_COUNT = 5;

// JPEG compression: larger values produce smaller, lower-quality photos.
static const int PHOTO_JPEG_QUALITY = 16;
// Default OV3660 path: capture an uncompressed VGA frame, then encode JPEG on the ESP32.
// Selecting Full HD restores the OV3660's native JPEG encoder.
static constexpr bool OV3660_FULL_HD = false;
static constexpr int OV3660_JPEG_QUALITY = 12; // Native sensor, smaller value = higher quality.
static constexpr uint8_t OV3660_SOFTWARE_JPEG_QUALITY = 78; // Software encoder, 1..100.

// Fixed Cloudflare Worker upload endpoint.
static const char LOCAL_DASHBOARD_UPLOAD_URL[] =
    "https://ee3070.panben218031.workers.dev/api/device/upload";

static const char DEVICE_ID[] = "esp32-camera-1";

// Audio: separate clocks for INMP441 and MAX98357; ESP32 Arduino core 3.3.12.
static const bool EVENT_AUDIO_ENABLED = true;
static const unsigned int EVENT_AUDIO_SECONDS = 5; // Recorded alongside the photo burst.
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
static const unsigned long TALK_WINDOW_MS = 90000; // Idle wait only; listening or speaking keeps the board awake.
static const unsigned long TALK_CONNECTION_GRACE_MS = 30000; // Stop stale playback after a lost connection.
