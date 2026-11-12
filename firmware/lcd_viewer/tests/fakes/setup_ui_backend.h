#pragma once

#include "native_display.h"
#include "../../src/board/board_port.h"
#include "../../src/board/jpeg_display.h"
#include "../../src/clock/viewer_clock.h"
#include "../../src/preview/preview_client.h"
#include "../../src/preview/session_state.h"
#include "../../src/ui/preview_ui.h"

#include <utility>

namespace native::setup {

uint32_t now = 0xfffff900;
bool wifi = false;
bool backlight = false;
bool bootPressed = false;
bool replaceAfterDecode = false;
unsigned startRequests = 0;
lcd::ViewerConfig bootConfig;
lcd::SetupNetworkSnapshot network;
lcd::SessionState session;
uint64_t pendingSequence = 0;
uint64_t lastSequence = 0;
uint32_t frameAt = 0;
const auto camera = cameraFixture();

void update(Display& display, uint32_t elapsed = 0) {
    now += elapsed;
    lcd::previewUiLoop(now);
    display.render();
}

void pressBoot(Display& display) {
    bootPressed = true;
    update(display, 1);
}

void setConfigured() {
    wifi = network.connected = true;
    std::strcpy(bootConfig.ssid, "Fixture network");
    std::strcpy(bootConfig.host, "192.0.2.51");
    std::strcpy(bootConfig.password, "fixture-password");
    std::strcpy(bootConfig.token, "fixture-token-0000000000");
    std::strcpy(network.ssid, bootConfig.ssid);
    std::strcpy(network.ip, "192.0.2.24");
    network.rssi = -53;
}

}

unsigned long millis() { return native::setup::now; }

namespace lcd {
using namespace native::setup;

void boardBacklight(bool on) { backlight = on; }
bool boardButtonPressed(uint32_t) { return std::exchange(bootPressed, false); }

void previewStart() {
    ++startRequests;
    if (!bootConfig.ready() || !session.start(now)) return;
    lastSequence = pendingSequence = 0;
}

void previewStop() { session.stop(); }
void previewExtend() { session.clock.extend(now); }

PreviewSnapshot previewSnapshot() {
    if (session.clock.expired(now)) session.stop();
    PreviewSnapshot snapshot;
    snapshot.wifiConfigured = bootConfig.wifiReady();
    snapshot.cameraConfigured = bootConfig.cameraReady();
    snapshot.configured = snapshot.wifiConfigured && snapshot.cameraConfigured;
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
    if (std::exchange(replaceAfterDecode, false)) {
        CHECK(session.forget(session.generation));
        lastSequence = 0;
    }
    return true;
}

const uint16_t* jpegPixels() { return camera.data(); }

ViewerConfig previewConfig() { return bootConfig; }
SetupNetworkSnapshot previewSetupSnapshot() { return network; }

// These boundaries are only link stubs for preview/menu tests. Scanning,
// persistence, restart and clock behavior are not covered by this harness.
bool previewRequestScan(uint32_t) { return false; }
void previewCancelScan(uint32_t) {}

bool saveConfig(const ViewerConfig&, ConfigError& error) {
    error = ConfigError::PrimaryWrite;
    return false;
}

const char* configErrorText(ConfigError) {
    return "Configuration writes are unavailable in preview tests";
}

void discardConfigSerialInput() {}
void restartAfterConfigSave() {}
ClockSnapshot viewerClockSnapshot() {
    ClockSnapshot snapshot;
    return snapshot;
}

}
