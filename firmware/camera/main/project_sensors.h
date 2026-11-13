#pragma once
#include "esp_camera.h"
void setupProjectSensors();
void pollProjectSensors();
bool setProjectBuzzer(bool on);
void captureProjectWakeBurst();
bool projectEventBusy();
