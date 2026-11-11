#pragma once
#include "../preview/preview_state.h"

namespace lcd {
enum class UiAction { Start, Extend, Stop };
using UiActionHandler = void (*)(UiAction);

void monitorViewBegin(UiActionHandler handler);
void monitorViewUpdate(const PreviewSnapshot& state, bool haveImage);
// The RGB565 pixels are borrowed until the next frame or clear, on the UI task.
void monitorViewSetFrame(const uint16_t* pixels);
void monitorViewClearFrame();
}
