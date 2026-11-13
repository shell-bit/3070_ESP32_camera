#pragma once
#include <Arduino.h>
void startProjectAudioRecording();
void finishProjectAudioRecording();
bool projectAudioPending();
void discardProjectAudio();
void uploadProjectAudio(const String &event);
// Call after event uploads. Starts the bounded listening window once per wake.
bool pollProjectAudioTalk();
