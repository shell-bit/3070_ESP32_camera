#include "src/board/board_port.h"
#include "src/config/viewer_config.h"
#include "src/preview/preview_client.h"
#include "src/ui/preview_ui.h"
#include "src/board/jpeg_display.h"
#include "src/clock/viewer_clock.h"

#if __has_include("camera_setup.local.h")
#include "camera_setup.local.h"
#define LCD_HAS_BUILT_CAMERA_CONFIG 1
#endif

bool ready=false;
void setup() {
    Serial.begin(115200);
    if(!psramFound()) { Serial.println("PSRAM missing: select ESP32S3 Dev Module / OPI PSRAM");return; }
    if(!lcd::boardBegin() || !lcd::jpegDisplayBegin()) { Serial.println("Display allocation/init failed");return; }
    lcd::ViewerConfig config;
    [[maybe_unused]] const bool loaded = lcd::loadConfig(config);
#ifdef LCD_HAS_BUILT_CAMERA_CONFIG
    if (loaded) {
        lcd::applyBuiltCameraConfig(config, lcd::cameraSetupJson, lcd::cameraSetupRevision);
    } else {
        Serial.println("Camera config deferred: no saved configuration could be read");
    }
#endif
    if(!lcd::previewClientBegin(config)) { Serial.println("Preview resources unavailable");return; }
    lcd::viewerClockBegin();lcd::previewUiBegin();ready=true;
    Serial.println("LCD preview ready. BOOT toggles viewing; USB serial accepts provisioning JSON while idle.");
    Serial.println(lcd::boardTouchAvailable()?"CST3530 detected":"CST3530 not found; BOOT remains usable");
}
void loop() {
    if(ready) {
        const uint32_t now=millis();
        lcd::viewerClockLoop(lcd::previewSnapshot().wifi);
        lcd::previewUiLoop(now);
        lcd::boardUiTick(now); // All LVGL calls, touch, decoding and LCD transfers belong to this task.
        if(!lcd::previewSnapshot().active) {
            if(lcd::previewUiAllowsSerial())lcd::pollConfigSerial();
            else lcd::discardConfigSerialInput();
        }
    }
    delay(5);
}
