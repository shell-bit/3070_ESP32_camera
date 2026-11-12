#pragma once
#include "preview_state.h"
#include "setup_network_state.h"
#include "../config/viewer_config.h"
namespace lcd {
bool previewClientBegin(const ViewerConfig& config);
ViewerConfig previewConfig();
bool previewRequestScan(uint32_t operationId);
void previewCancelScan(uint32_t operationId);
SetupNetworkSnapshot previewSetupSnapshot();
void previewStart();
void previewStop();
void previewExtend();
PreviewSnapshot previewSnapshot();
bool previewDecodeLatest(); // UI task only; decodes to jpegPixels().
}
