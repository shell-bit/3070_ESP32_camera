#include "src/board/board_port.h"
#include "src/config/viewer_config.h"
#include "src/preview/preview_client.h"
#include "src/ui/preview_ui.h"
#include "src/board/jpeg_display.h"

bool ready=false;
void setup() {
    Serial.begin(115200);
    if(!psramFound()) { Serial.println("PSRAM missing: select ESP32S3 Dev Module / OPI PSRAM");return; }
    if(!lcd::boardBegin() || !lcd::jpegDisplayBegin()) { Serial.println("Display allocation/init failed");return; }
    lcd::ViewerConfig config;
    if(!lcd::loadConfig(config))config=lcd::ViewerConfig{};
    if(!lcd::previewClientBegin(config)) { Serial.println("Preview resources unavailable");return; }
    lcd::previewUiBegin();ready=true;
    Serial.println("LCD preview ready. BOOT toggles viewing; USB serial accepts provisioning JSON while idle.");
    Serial.println(lcd::boardTouchAvailable()?"CST3530 detected":"CST3530 not found; BOOT remains usable");
}
void loop() {
    if(ready) {
        const uint32_t now=millis();
        lcd::previewUiLoop(now);
        lcd::boardUiTick(now); // All LVGL calls, touch, decoding and LCD transfers belong to this task.
        if(!lcd::previewSnapshot().active)lcd::pollConfigSerial();
    }
    delay(5);
}
