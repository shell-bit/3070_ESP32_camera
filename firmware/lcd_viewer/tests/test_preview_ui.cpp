#include "fakes/native_display.h"
#include "../src/board/board_port.h"
#include "../src/board/jpeg_display.h"
#include "../src/preview/preview_client.h"
#include "../src/ui/preview_ui.h"
#include "../src/preview/session_state.h"

#include <utility>

namespace {
uint32_t now = 0xfffff900;
bool configured = false;
bool wifi = false;
bool backlight = false;
bool bootPressed = false;
bool replaceAfterDecode = false;
unsigned startRequests = 0;
lcd::SessionState session;
uint64_t pendingSequence = 0;
uint64_t lastSequence = 0;
uint32_t frameAt = 0;
const auto camera = native::cameraFixture();

void update(native::Display& display, uint32_t elapsed = 0) {
    now += elapsed;
    lcd::previewUiLoop(now);
    display.render();
}

void pressBoot(native::Display& display) {
    bootPressed = true;
    update(display, 1);
}
}

unsigned long millis() { return now; }

namespace lcd {

void boardBacklight(bool on) { backlight = on; }
bool boardButtonPressed(uint32_t) { return std::exchange(bootPressed, false); }

void previewStart() {
    ++startRequests;
    if (!configured || !session.start(now)) return;
    lastSequence = pendingSequence = 0;
}

void previewStop() { session.stop(); }
void previewExtend() { session.clock.extend(now); }

PreviewSnapshot previewSnapshot() {
    if (session.clock.expired(now)) session.stop();
    PreviewSnapshot snapshot;
    snapshot.configured = configured;
    snapshot.wifi = wifi;
    snapshot.active = session.clock.active();
    snapshot.generation = session.generation;
    snapshot.remainingMs = session.clock.remaining(now);
    snapshot.stale = session.clock.stale(now);
    snapshot.sequence = lastSequence;
    snapshot.frameAgeMs = lastSequence ? now - frameAt : 0;
    if (snapshot.active) std::strcpy(snapshot.state, lastSequence ? "streaming" : "starting");
    return snapshot;
}

bool previewDecodeLatest() {
    if (!pendingSequence) return false;
    lastSequence = std::exchange(pendingSequence, 0);
    frameAt = now;
    session.clock.frame(now);
    // Simulate the client worker replacing the session immediately after a successful decode.
    if (std::exchange(replaceAfterDecode, false)) {
        CHECK(session.forget(session.generation));
        lastSequence = 0;
    }
    return true;
}

const uint16_t* jpegPixels() { return camera.data(); }

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
        update(display, 3100);
        CHECK(backlight);
        CHECK(now < 3100); // Crossed the uint32 millisecond boundary.
        CHECK(native::findLabel("Setup required"));
        display.tap(native::findButton("Setup"));
        pressBoot(display);
        CHECK(!session.clock.active());
        CHECK(native::findLabel("115200", false));
        std::puts("PASS unconfigured backlight stays on across millis wrap");
        return 0;
    }
    CHECK(argc == 1);
    configured = wifi = true;
    lcd::previewUiBegin();
    CHECK(backlight);
    update(display, 100);
    display.tap(native::findButton("Status"));
    update(display, 2800);
    CHECK(backlight);
    display.tap(native::findButton("Setup"));
    update(display, 200);
    CHECK(!backlight);
    CHECK(native::findLabel("115200", false));
    std::puts("PASS paging does not reset the configured backlight cutoff across millis wrap");

    display.tap(native::findButton("Status"));
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
    update(display, 2900);
    CHECK(backlight);
    update(display, 201);
    CHECK(!backlight);
    std::puts("PASS Close returns to Home and starts the three-second backlight deadline");

    display.tap(native::findButton("Setup"));
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
    update(display, 3100);
    CHECK(!backlight);
    std::puts("PASS BOOT from Setup, Continue from click time and automatic expiry return Home");

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
