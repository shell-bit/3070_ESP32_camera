#pragma once
#include <Arduino.h>
void setupProjectAiAlarm();
void beginProjectAiEvent(const String &event, int capturedCount);
// Call once uploads have completed or timed out. True means stay awake.
bool pollProjectAiAlarm();
