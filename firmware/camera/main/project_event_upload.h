#pragma once
#include "project_camera_manager.h"
enum class UploadResult { Saved, Retry, Rejected };
UploadResult uploadEventFrame(const OwnedJpeg &, const String &metadata, const String &event,
                              int index, bool night, unsigned long remaining);
String projectApiUrl(const char *path);
