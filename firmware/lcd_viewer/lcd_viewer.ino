#include "src/board/board_port.h"
#include "src/config/viewer_config.h"
#include "src/preview/preview_client.h"
#include "src/ui/preview_ui.h"
#include "src/board/jpeg_display.h"
#include "src/clock/viewer_clock.h"

extern "C" const char lcd_setup_json[];
extern "C" const uint32_t lcd_setup_size;

bool ready=false;
void setup() {
    Serial.begin(115200);
    if(!psramFound()) { Serial.println("PSRAM missing: select ESP32S3 Dev Module / OPI PSRAM");return; }
    if(!lcd::boardBegin() || !lcd::jpegDisplayBegin()) { Serial.println("Display allocation/init failed");return; }
    lcd::ViewerConfig config;
    lcd::loadConfig(config);
    if (lcd_setup_size != 0) {
        lcd::applyBuiltSetupConfig(config, lcd_setup_json, lcd_setup_size);
    }
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
