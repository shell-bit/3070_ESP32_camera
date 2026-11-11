#pragma once
#include <Arduino.h>
#include <lvgl.h>
namespace lcd {
constexpr uint16_t kWidth=320, kHeight=240;
bool boardBegin();
void boardBacklight(bool on);
bool boardButtonPressed(uint32_t now);
void boardUiTick(uint32_t now);
bool boardTouchAvailable();
}
