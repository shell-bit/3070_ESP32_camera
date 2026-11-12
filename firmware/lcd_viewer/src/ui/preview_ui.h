#pragma once
#include <cstdint>
namespace lcd {
void previewUiBegin();
void previewUiLoop(uint32_t now);
bool previewUiAllowsSerial();
}
