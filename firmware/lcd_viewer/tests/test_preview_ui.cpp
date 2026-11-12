#include "fakes/setup_ui_backend.h"

using namespace native::setup;

namespace {
constexpr uint32_t kExpectedIdleBacklightMs = 30000;
}

int main(int argc, char** argv) {
    native::Display display;
    if (argc == 2 && std::strcmp(argv[1], "unconfigured") == 0) {
        lcd::previewUiBegin();
        CHECK(backlight);
        update(display, 101);
        if (auto* start = native::findButton("Start preview")) display.tap(start);
        CHECK(startRequests == 0);
        CHECK(!session.clock.active());
        std::puts("PASS unconfigured touch cannot request a preview");
        update(display, kExpectedIdleBacklightMs + 100);
        CHECK(backlight);
        CHECK(now < kExpectedIdleBacklightMs + 100); // Crossed the uint32 millisecond boundary.
        CHECK(native::findLabel("Setup required"));
        display.tap(native::findButton("Setup"));
        CHECK(native::findButton("Back"));
        pressBoot(display);
        CHECK(!session.clock.active());
        CHECK(startRequests == 0);
        CHECK(native::findLabel("Setup required"));
        std::puts("PASS unconfigured backlight stays on across millis wrap");
        return 0;
    }
    CHECK(argc == 1);
    setConfigured();
    lcd::previewUiBegin();
    CHECK(backlight);
    update(display, 100);
    display.tap(native::findButton("Status"));
    update(display, kExpectedIdleBacklightMs - 101);
    CHECK(backlight);
    CHECK(now < kExpectedIdleBacklightMs); // Crossed the uint32 millisecond boundary.
    update(display, 1);
    CHECK(!backlight);
    std::puts("PASS idle backlight cutoff follows the 30-second deadline across millis wrap");

    pressBoot(display);
    CHECK(session.clock.active());
    CHECK(backlight);
    CHECK(!native::findButton("Status"));
    CHECK(!native::frameImage());
    CHECK(native::findLabel("CONNECTING"));
    pendingSequence = 13;
    update(display, 100);
    CHECK(native::frameImage());
    CHECK(native::findLabel("LIVE"));
    std::puts("PASS BOOT from Status opens the monitor and only a decoded frame becomes live");

    const uint32_t previousGeneration = session.generation;
    CHECK(session.forget(previousGeneration));
    lastSequence = 0;
    update(display, 1);
    CHECK(!native::frameImage());
    CHECK(!native::findLabel("LIVE"));
    pendingSequence = 1;
    update(display, 100);
    CHECK(native::frameImage());
    CHECK(native::findLabel("LIVE"));
    std::puts("PASS generation change removes the old frame immediately");

    replaceAfterDecode = true;
    pendingSequence = 2;
    update(display, 1);
    CHECK(!native::frameImage());
    CHECK(native::findLabel("CONNECTING"));
    std::puts("PASS a decoded frame cannot cross a generation change during decode");

    display.tap(native::findButton("Close"));
    update(display, 1);
    CHECK(!session.clock.active());
    CHECK(!native::frameImage());
    CHECK(native::findButton("Home"));
    CHECK(native::findLabel("Ready"));
    CHECK(backlight);
    update(display, kExpectedIdleBacklightMs - 2);
    CHECK(backlight);
    update(display, 1);
    CHECK(!backlight);
    std::puts("PASS Close returns to Home and starts the 30-second backlight deadline");

    display.tap(native::findButton("Setup"));
    CHECK(backlight);
    pressBoot(display);
    CHECK(!session.clock.active());
    CHECK(native::findButton("Home"));
    pressBoot(display);
    CHECK(session.clock.active());
    CHECK(backlight);
    CHECK(!native::findButton("Setup"));
    update(display, 10000);
    display.tap(native::findButton("Continue 60s"));
    CHECK(session.clock.remaining(now) == 60000);
    update(display, 59900);
    CHECK(session.clock.active());
    CHECK(backlight);
    update(display, 100);
    CHECK(!session.clock.active());
    CHECK(native::findLabel("Ready"));
    CHECK(backlight);
    update(display, kExpectedIdleBacklightMs - 1);
    CHECK(backlight);
    update(display, 1);
    CHECK(!backlight);
    std::puts("PASS BOOT cancels Setup, then Continue from click time and expiry return Home");

    wifi = false;
    update(display, 100);
    CHECK(native::findLabel("Wi-Fi offline"));
    display.tap(native::findButton("Start preview"));
    update(display, 1);
    CHECK(session.clock.active());
    CHECK(!native::findLabel("LIVE"));
    CHECK(native::findLabel("Wi-Fi reconnecting", false));
    pressBoot(display);
    CHECK(!session.clock.active());
    CHECK(native::findButton("Home"));
    CHECK(native::findLabel("Wi-Fi offline"));
    std::puts("PASS configured offline Start enters retry and BOOT stops it");

}
