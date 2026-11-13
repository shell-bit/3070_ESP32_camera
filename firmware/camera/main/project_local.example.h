#pragma once
#define PROJECT_WIFI_SSID "your-2.4GHz-network"
#define PROJECT_WIFI_PASSWORD "your-password"
#define PROJECT_DEVICE_TOKEN "registered-device-token"
#define PROJECT_DEVICE_ID "esp32-camera-1"
#define PROJECT_UPLOAD_URL "https://your-server.example/api/device/upload"
// At least 24 characters. Same credential on the LCD. Empty disables preview HTTP.
#define PROJECT_PREVIEW_TOKEN "replace-with-random-pairing-token"
// --identity supplies the model/V2 flags; do not override them in this file.
#define PROJECT_DEEP_SLEEP 0
