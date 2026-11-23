#pragma once
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
void startProjectAudioRecording();
void finishProjectAudioRecording();
bool projectAudioPending();
bool projectAudioRecording();
size_t projectAudioPendingBytes();
void discardProjectAudio();
void uploadProjectAudio(const String &event, HTTPClient &http, WiFiClientSecure &client, bool &acceptsDeflate);
// Call after uploads. Active sessions extend the window; idle time remains bounded.
bool pollProjectAudioTalk();
void reportProjectAudioState(const char *state);
