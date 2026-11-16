#pragma once
#include <Arduino.h>
void startProjectAudioRecording();
void finishProjectAudioRecording();
bool projectAudioPending();
void discardProjectAudio();
void uploadProjectAudio(const String &event);
// Call after uploads. Active sessions extend the window; idle time remains bounded.
bool pollProjectAudioTalk();
void reportProjectAudioState(const char *state);
