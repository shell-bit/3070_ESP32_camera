#pragma once
// Copy project_local.example.h to project_local.h; never commit credentials.
#if __has_include("project_local.h")
#include "project_local.h"
#endif
#ifndef PROJECT_WIFI_SSID
#define PROJECT_WIFI_SSID ""
#define PROJECT_WIFI_PASSWORD ""
#endif
#ifndef PROJECT_DEVICE_TOKEN
#define PROJECT_DEVICE_TOKEN ""
#endif
#ifndef PROJECT_DEVICE_ID
#define PROJECT_DEVICE_ID "esp32-camera-1"
#endif
#ifndef PROJECT_UPLOAD_URL
#define PROJECT_UPLOAD_URL ""
#endif
#ifndef PROJECT_PREVIEW_TOKEN
#define PROJECT_PREVIEW_TOKEN ""
#endif
#ifndef PROJECT_DEEP_SLEEP
#define PROJECT_DEEP_SLEEP 0
#endif
#ifndef PROJECT_FACE_ENABLED
#define PROJECT_FACE_ENABLED 0
#endif
#ifndef PROJECT_UPLOAD_V2
#define PROJECT_UPLOAD_V2 0
#endif
#ifndef PROJECT_FACE_ALLOW_FORMAT
#define PROJECT_FACE_ALLOW_FORMAT 0
#endif
#ifndef PROJECT_ENROLLMENT_MIN_SIMILARITY
#define PROJECT_ENROLLMENT_MIN_SIMILARITY 0.75f
#endif
