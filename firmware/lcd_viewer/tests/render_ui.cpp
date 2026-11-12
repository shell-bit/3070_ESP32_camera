#include "fakes/setup_ui_backend.h"
#include "../src/ui/monitor_view.h"

#include <fstream>
#include <limits>

namespace {

using lcd::PreviewSnapshot;
using lcd::UiAction;

std::vector<UiAction> actions;

void dispatch(UiAction action) { actions.push_back(action); }

lv_area_t boundsOf(lv_obj_t* object) {
    CHECK(object != nullptr);
    lv_area_t bounds;
    lv_obj_get_coords(object, &bounds);
    CHECK(bounds.x1 >= 0 && bounds.x2 < native::width);
    CHECK(bounds.y1 >= 0 && bounds.y2 < native::height);
    return bounds;
}

lv_area_t buttonBounds(const char* text) {
    auto* button = native::findButton(text);
    const auto bounds = boundsOf(button);
    CHECK(lv_area_get_height(&bounds) == 44);
    const auto label = boundsOf(native::findLabel(text));
    CHECK(label.x1 >= bounds.x1 && label.x2 <= bounds.x2);
    CHECK(label.y1 >= bounds.y1 && label.y2 <= bounds.y2);
    return bounds;
}

void checkSeparate(const lv_area_t& a, const lv_area_t& b) {
    CHECK(a.x2 < b.x1 || b.x2 < a.x1 || a.y2 < b.y1 || b.y2 < a.y1);
}

void checkTextFits(lv_obj_t* label, bool wrap = false) {
    CHECK(label);
    lv_point_t size;
    lv_txt_get_size(&size, lv_label_get_text(label),
        lv_obj_get_style_text_font(label, LV_PART_MAIN),
        lv_obj_get_style_text_letter_space(label, LV_PART_MAIN),
        lv_obj_get_style_text_line_space(label, LV_PART_MAIN),
        wrap ? lv_obj_get_content_width(label) : LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    CHECK(size.x <= lv_obj_get_content_width(label));
    CHECK(size.y <= lv_obj_get_content_height(label));
}

void screenshot(const native::Display& display, const std::string& directory, const char* name) {
    std::ofstream file(directory + "/" + name + ".rgb565", std::ios::binary);
    CHECK(file.good());
    for (uint16_t color : display.pixels()) {
        file.put(static_cast<char>(color & 0xff));
        file.put(static_cast<char>(color >> 8));
    }
    file.close();
    CHECK(file.good());
}

void requireMonitor(const char* status, const char* detail, bool haveImage) {
    auto* statusLabel = native::findLabel(status);
    CHECK(statusLabel);
    CHECK(boundsOf(statusLabel).y2 < 26);
    if (std::strcmp(status, "LIVE") != 0) CHECK(!native::findLabel("LIVE"));
    if (detail) CHECK(native::findLabel(detail, false));
    CHECK((native::frameImage() != nullptr) == haveImage);
    CHECK(!native::findButton("Home"));
    CHECK(!native::findButton("Status"));
    CHECK(!native::findButton("Setup"));
}

void renderSetup(native::Display& display, const std::string& output) {
    using namespace native::setup;
    lv_obj_clean(lv_scr_act());
    setConfigured();
    lcd::previewUiBegin();
    update(display);
    display.tap(native::findButton("Setup"));
    update(display, 1);
    CHECK(native::findButton("Back"));
    screenshot(display, output, "03-setup");
    pressBoot(display);
    CHECK(native::findButton("Home"));
}

}

int main(int argc, char** argv) {
    CHECK(argc == 2);
    const std::string output(argv[1]);
    native::Display display;
    const auto camera = native::cameraFixture();
    lcd::monitorViewBegin(dispatch);

    auto show = [&](const PreviewSnapshot& snapshot, bool haveImage, const char* name) {
        lcd::monitorViewUpdate(snapshot, haveImage);
        display.render();
        screenshot(display, output, name);
    };

    PreviewSnapshot home;
    home.configured = home.wifi = home.wifiConfigured = home.cameraConfigured = true;
    show(home, false, "01-home");
    CHECK(native::findLabel("Ready"));
    CHECK(native::findLabel("Preview is off"));
    const auto start = buttonBounds("Start preview");
    const auto homeTab = buttonBounds("Home");
    const auto statusTab = buttonBounds("Status");
    const auto setupTab = buttonBounds("Setup");
    CHECK(homeTab.y1 == 196 && statusTab.y1 == 196 && setupTab.y1 == 196);
    checkSeparate(homeTab, statusTab);
    checkSeparate(statusTab, setupTab);
    checkSeparate(start, homeTab);
    checkSeparate(start, statusTab);
    checkSeparate(start, setupTab);
    CHECK(!native::frameImage());
    CHECK(!native::findLabel("LIVE"));
    display.tap(native::findButton("Status"));
    CHECK(!native::findButton("Start preview"));
    CHECK(native::findLabel("Wi-Fi", false));
    CHECK(native::findLabel("Idle backlight"));
    screenshot(display, output, "02-status");
    display.tap(native::findButton("Setup"));
    CHECK(!native::findButton("Start preview"));
    CHECK(actions == std::vector<UiAction>{UiAction::Setup});
    actions.clear();
    display.tap(native::findButton("Home"));
    CHECK(actions.empty());
    CHECK(native::findLabel("Ready"));
    std::puts("PASS Home, Status and Setup tab dispatch and 44px touch targets");

    PreviewSnapshot unconfigured;
    show(unconfigured, false, "04-unconfigured");
    CHECK(native::findLabel("Setup required"));
    CHECK(!native::findLabel("Ready"));
    if (auto* button = native::findButton("Start preview")) {
        CHECK(lv_obj_has_state(button, LV_STATE_DISABLED));
        display.tap(button);
    }
    CHECK(actions.empty());

    auto offline = home;
    offline.wifi = false;
    show(offline, false, "05-offline");
    CHECK(native::findLabel("Wi-Fi offline"));
    CHECK(!native::findLabel("Ready"));
    CHECK(!native::findLabel("LIVE"));
    display.tap(native::findButton("Start preview"));
    CHECK(actions == std::vector<UiAction>{UiAction::Start});
    std::puts("PASS unconfigured Start is blocked and configured offline Start can request retry");

    auto state = home;
    state.active = true;
    state.generation = 1;
    state.remainingMs = 58200;
    state.stale = false; // A fresh network status alone does not mean a frame was decoded.
    std::strcpy(state.state, "starting");
    show(state, false, "06-waiting-first-frame");
    requireMonitor("CONNECTING", nullptr, false);

    std::strcpy(state.state, "streaming");
    state.sequence = 123456;
    state.frameAgeMs = 47;
    lcd::monitorViewSetFrame(camera.data());
    show(state, true, "07-live");
    requireMonitor("LIVE", nullptr, true);
    auto* image = native::frameImage();
    const auto imageBounds = boundsOf(image);
    CHECK(imageBounds.x1 == 0 && imageBounds.y1 == 0);
    CHECK(imageBounds.x2 == 319 && imageBounds.y2 == 239);
    CHECK(lv_img_get_zoom(image) == LV_IMG_ZOOM_NONE);
    const auto* descriptor = static_cast<const lv_img_dsc_t*>(lv_img_get_src(image));
    CHECK(descriptor->header.w == 320 && descriptor->header.h == 240);
    CHECK(descriptor->data_size == 320 * 240 * 2);
    // Check the entire uncovered video area including both horizontal edges.
    for (int i = 26 * native::width; i < 178 * native::width; ++i) {
        CHECK(display.pixels()[i] == camera[i]);
    }
    CHECK(native::findLabel("123456", false));
    CHECK(native::findLabel("47ms", false));
    CHECK(native::findLabel("59s", false));
    const auto extend = buttonBounds("Continue 60s");
    const auto close = buttonBounds("Close");
    CHECK(extend.y1 == 196 && close.y1 == 196);
    checkSeparate(extend, close);
    display.tap(native::findButton("Continue 60s"));
    CHECK(actions.size() == 2 && actions.back() == UiAction::Extend);
    std::puts("PASS first-frame gating and native-scale video between the overlay rows");

    for (const auto& pause : {
        std::array<const char*, 3>{"paused_for_event", "Capturing event", "08-paused-event"},
        std::array<const char*, 3>{"paused_for_enrollment", "Member enrollment", "09-paused-enrollment"},
        std::array<const char*, 3>{"paused_for_identity", "Identity processing", "10-paused-identity"},
    }) {
        std::strcpy(state.state, pause[0]);
        show(state, true, pause[2]);
        requireMonitor("PAUSED", pause[1], true);
        CHECK(native::findLabel("Last frame shown."));
    }
    std::strcpy(state.state, "streaming");
    state.wifi = false;
    show(state, true, "11-wifi-reconnecting");
    requireMonitor("PAUSED", "Wi-Fi reconnecting", true);
    state.wifi = true;
    state.stale = true;
    state.frameAgeMs = 1000;
    show(state, true, "12-stale");
    requireMonitor("PAUSED", "Waiting for a fresh frame", true);
    state.stale = false;
    std::strcpy(state.state, "error");
    show(state, true, "13-camera-error");
    requireMonitor("ERROR", "Camera error", true);

    // A stream/client error also takes precedence when the last image is fresh.
    std::strcpy(state.state, "streaming");
    std::strcpy(state.error, "Camera reply could not be read while the control connection was restarting now.");
    state.sequence = std::numeric_limits<uint64_t>::max();
    state.frameAgeMs = std::numeric_limits<uint32_t>::max();
    show(state, true, "14-extreme-text");
    requireMonitor("ERROR", "Camera reply", true);
    auto* errorText = native::findLabel("Camera reply", false);
    const auto error = boundsOf(errorText);
    CHECK(error.y1 >= 26 && error.y2 < 178);
    checkTextFits(errorText, true);
    checkSeparate(error, extend);
    checkSeparate(error, close);
    auto* sequence = native::findLabel("18446744073709551615", false);
    auto* age = native::findLabel("4294967295ms", false);
    for (auto* metadata : {sequence, age}) {
        const auto line = boundsOf(metadata);
        CHECK(line.y1 >= 178 && line.y2 < 196);
        checkSeparate(line, extend);
        checkSeparate(line, close);
        checkTextFits(metadata);
    }
    if (sequence != age) checkSeparate(boundsOf(sequence), boundsOf(age));
    std::puts("PASS pause and error priority, retained image and extreme text bounds");

    const uint16_t oldCenter = display.pixels()[120 * native::width + 160];
    lcd::monitorViewClearFrame();
    ++state.generation;
    state.error[0] = '\0';
    std::strcpy(state.state, "starting");
    show(state, false, "15-new-generation");
    requireMonitor("CONNECTING", nullptr, false);
    CHECK(!native::findLabel("18446744073709551615", false));
    CHECK(display.pixels()[120 * native::width + 160] != oldCenter);
    display.tap(native::findButton("Close"));
    CHECK(actions.size() == 3 && actions.back() == UiAction::Stop);
    show(home, false, "16-stopped");
    CHECK(native::findLabel("Ready"));
    CHECK(!native::frameImage());
    CHECK(!native::findButton("Close"));
    std::puts("PASS generation clear removes the old pixels and Close returns to Home");
    renderSetup(display, output);
}
