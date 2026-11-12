#include "preview_ui.h"
#include "../board/board_port.h"
#include "../board/jpeg_display.h"
#include "monitor_view.h"
#include "setup_view.h"
#include "../clock/viewer_clock.h"
#include "../preview/preview_client.h"
#include <cstring>

namespace lcd {
namespace {
constexpr uint32_t kIdleBacklightMs = 30000;
constexpr uint32_t kTextUpdateMs = 100;
uint32_t seenGeneration = 0, idleSince = 0, lastUpdate = 0;
bool haveImage = false, wasActive = false, backlightOn = false;
PreviewSnapshot presented;

void observeSession(const PreviewSnapshot& state, uint32_t now) {
    if (state.generation != seenGeneration || (!state.active && haveImage)) {
        seenGeneration = state.generation;
        haveImage = false;
        monitorViewClearFrame();
    }
    if (wasActive && !state.active) idleSince = now;
    wasActive = state.active;
}

void updateBacklight(const PreviewSnapshot& state, uint32_t now) {
    const bool on = state.active || setupViewVisible() || !state.wifiConfigured || now - idleSince < kIdleBacklightMs;
    if (on != backlightOn) {
        boardBacklight(on);
        backlightOn = on;
    }
}

void updateView(const PreviewSnapshot& state, uint32_t now, bool force = false) {
    // Apply mode changes immediately; the readouts need only update at 10 Hz.
    if (!force && now - lastUpdate < kTextUpdateMs && state.active == presented.active &&
        state.configured == presented.configured && state.wifi == presented.wifi &&
        state.wifiConfigured == presented.wifiConfigured && state.cameraConfigured == presented.cameraConfigured &&
        state.generation == presented.generation && state.stale == presented.stale &&
        std::strcmp(state.state, presented.state) == 0 && std::strcmp(state.error, presented.error) == 0)
        return;
    monitorViewUpdate(state, haveImage, viewerClockSnapshot());
    presented = state;
    lastUpdate = now;
}

void runAction(UiAction action) {
    switch (action) {
        case UiAction::Start:
            if (!setupViewOwnsDraft() && !setupViewScanBusy() && previewSnapshot().configured) previewStart();
            break;
        case UiAction::Extend: previewExtend(); break;
        case UiAction::Stop: previewStop(); break;
        case UiAction::Setup: {
            const auto state = previewSnapshot();
            if (!state.active && !setupViewVisible() && !setupViewOwnsDraft()) {
                monitorViewShowSetup(true);
                setupViewOpen(millis(), state.configured);
            }
            break;
        }
    }
}

void touchAction(UiAction action) {
    runAction(action);
    const auto now = millis();
    const auto state = previewSnapshot();
    observeSession(state, now);
    updateBacklight(state, now);
    updateView(state, now, true);
}

void setupExited(SetupExit exit) {
    monitorViewShowSetup(false);
    idleSince = millis();
    if (exit == SetupExit::StartPreview) touchAction(UiAction::Start);
    else {
        const auto state = previewSnapshot();
        updateBacklight(state, idleSince);
        updateView(state, idleSince, true);
    }
}
}

void previewUiBegin() {
    monitorViewBegin(touchAction);
    setupViewBegin(setupExited);
    idleSince = millis();
    haveImage = wasActive = false;
    boardBacklight(true);
    backlightOn = true;
    const auto state = previewSnapshot();
    observeSession(state, idleSince);
    updateView(state, idleSince, true);
}

void previewUiLoop(uint32_t now) {
    auto state = previewSnapshot();
    observeSession(state, now);
    const bool setupWasVisible = setupViewVisible();
    setupViewLoop(now, viewerClockSnapshot());
    if (boardButtonPressed(now)) {
        if (setupWasVisible) setupViewBack(now);
        else if (state.active) runAction(UiAction::Stop);
        else if (state.configured) runAction(UiAction::Start);
        state = previewSnapshot();
        observeSession(state, now);
        boardBacklight(true);
        backlightOn = true;
        idleSince = now;
    }

    bool firstPicture = false;
    if (state.active) {
        const uint32_t decodingGeneration = state.generation;
        const bool decoded = previewDecodeLatest();
        // The client may have expired or replaced the session during decoding.
        // Recheck before LVGL can draw either buffer.
        state = previewSnapshot();
        observeSession(state, now);
        if (decoded && state.active && decodingGeneration == state.generation) {
            firstPicture = !haveImage;
            haveImage = true;
            monitorViewSetFrame(jpegPixels());
        }
    }
    updateBacklight(state, now);
    updateView(state, now, firstPicture);
}

bool previewUiAllowsSerial() {
    return !setupViewOwnsDraft() && !setupViewScanBusy() && !previewSnapshot().active;
}
}
