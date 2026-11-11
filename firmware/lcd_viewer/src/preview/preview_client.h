#pragma once
#include "preview_state.h"
#include "../config/viewer_config.h"
namespace lcd {
bool previewClientBegin(const ViewerConfig& config);
void previewStart();
void previewStop();
void previewExtend();
PreviewSnapshot previewSnapshot();
bool previewDecodeLatest(); // UI task only; decodes to jpegPixels().
}
