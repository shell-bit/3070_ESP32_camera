#include "monitor_view.h"
#include <lvgl.h>
#include <cstdio>
#include <cstring>

extern "C" const lv_font_t monitor_font_28;

namespace lcd {
namespace {
constexpr lv_coord_t kWidth = 320;
constexpr lv_coord_t kHeight = 240;
constexpr lv_coord_t kHeaderHeight = 42;
constexpr lv_coord_t kTouchHeight = 44;
constexpr lv_coord_t kLiveHeaderHeight = 26;
constexpr lv_coord_t kMetadataHeight = 18;
constexpr lv_coord_t kActionsY = kHeight - kTouchHeight;
constexpr lv_coord_t kMetadataY = kActionsY - kMetadataHeight;
constexpr lv_opa_t kNoticeOpacity = 242;
constexpr lv_coord_t kNoticeTextWidth = 246;
constexpr lv_coord_t kErrorTextY = 51;
constexpr lv_coord_t kMaxErrorTextHeight = 72;

constexpr uint32_t kBase = 0x1e1e2e, kCrust = 0x11111b, kSurface = 0x313244;
constexpr uint32_t kText = 0xcdd6f4, kMuted = 0xa6adc8, kBlue = 0x89b4fa;
constexpr uint32_t kPeach = 0xfab387, kGreen = 0xa6e3a1, kYellow = 0xf9e2af, kRed = 0xf38ba8;

enum Page { Home, Status, Setup, PageCount };
constexpr lv_coord_t kNavX[] = {0, 106, 213};
constexpr lv_coord_t kNavW[] = {106, 107, 107};

struct View {
    lv_obj_t *dashboard, *monitor, *pages[PageCount], *nav[PageCount], *navLabels[PageCount], *underline;
    lv_obj_t *wifi, *hero, *homeDetail, *hint, *start, *startText, *play, *statusValues[4];
    lv_obj_t *image, *stateDot, *mode, *remaining, *frame, *age, *extend, *stop;
    lv_obj_t *notice, *noticeTitle, *noticePicture, *noticeDetail;
    lv_coord_t noticeDetailHeight;
    lv_img_dsc_t picture;
    UiActionHandler handler;
    bool active, configured;
};

View ui{};

void visible(lv_obj_t* object, bool show) {
    if (show == !lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return;
    if (show) lv_obj_clear_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}

void literal(lv_obj_t* object, const char* text) {
    if (std::strcmp(lv_label_get_text(object), text) != 0) lv_label_set_text_static(object, text);
}

bool copyText(lv_obj_t* object, const char* text) {
    if (std::strcmp(lv_label_get_text(object), text) == 0) return false;
    lv_label_set_text(object, text);
    return true;
}

void textColor(lv_obj_t* object, uint32_t hex) {
    const auto color = lv_color_hex(hex);
    if (lv_obj_get_style_text_color(object, LV_PART_MAIN).full != color.full)
        lv_obj_set_style_text_color(object, color, 0);
}

void background(lv_obj_t* object, uint32_t hex) {
    const auto color = lv_color_hex(hex);
    if (lv_obj_get_style_bg_color(object, LV_PART_MAIN).full != color.full)
        lv_obj_set_style_bg_color(object, color, 0);
}

lv_obj_t* panel(lv_obj_t* parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
                uint32_t fill = kBase, lv_opa_t opacity = LV_OPA_COVER) {
    auto* object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(object, w, h);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_style_bg_color(object, lv_color_hex(fill), 0);
    lv_obj_set_style_bg_opa(object, opacity, 0);
    return object;
}

lv_obj_t* label(lv_obj_t* parent, const char* text, const lv_font_t& font,
                uint32_t color = kText, lv_coord_t x = 0, lv_coord_t y = 0,
                lv_coord_t width = LV_SIZE_CONTENT, lv_text_align_t alignment = LV_TEXT_ALIGN_LEFT) {
    auto* object = lv_label_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_width(object, width);
    lv_obj_set_style_text_font(object, &font, 0);
    lv_obj_set_style_text_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(object, alignment, 0);
    lv_label_set_text_static(object, text);
    return object;
}

lv_obj_t* button(lv_obj_t* parent, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                 uint32_t color, uint32_t pressed, lv_event_cb_t callback) {
    auto* object = lv_btn_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(object, w, kTouchHeight);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(pressed), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(object, callback, LV_EVENT_CLICKED, nullptr);
    return object;
}

void selectPage(Page page) {
    for (int i = 0; i < PageCount; ++i) {
        visible(ui.pages[i], i == page);
        textColor(ui.navLabels[i], i == page ? kPeach : kText);
    }
    lv_obj_set_x(ui.underline, kNavX[page]);
    lv_obj_set_width(ui.underline, kNavW[page]);
}

void navigate(lv_event_t* event) {
    if (ui.active) return;
    for (int i = 0; i < PageCount; ++i) {
        if (lv_event_get_target(event) == ui.nav[i]) {
            selectPage(static_cast<Page>(i));
            return;
        }
    }
}

void action(lv_event_t* event) {
    if (!ui.handler) return;
    const auto* target = lv_event_get_target(event);
    if (target == ui.start && !ui.active && ui.configured) ui.handler(UiAction::Start);
    else if (target == ui.extend && ui.active) ui.handler(UiAction::Extend);
    else if (target == ui.stop && ui.active) ui.handler(UiAction::Stop);
}

void cameraMark(lv_obj_t* parent) {
    auto* body = panel(parent, 27, 22, 31, 29, kBase, LV_OPA_TRANSP);
    lv_obj_set_style_border_width(body, 2, 0);
    lv_obj_set_style_border_color(body, lv_color_hex(kBlue), 0);
    lv_obj_set_style_radius(body, 5, 0);

    static const lv_point_t lens[] = {{0, 10}, {15, 1}, {15, 24}, {0, 15}};
    auto* line = lv_line_create(parent);
    lv_obj_remove_style_all(line);
    lv_obj_set_pos(line, 57, 24);
    lv_line_set_points(line, lens, sizeof lens / sizeof lens[0]);
    lv_obj_set_style_line_color(line, lv_color_hex(kBlue), 0);
    lv_obj_set_style_line_width(line, 2, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
}

void dashboard(lv_obj_t* screen) {
    ui.dashboard = panel(screen, 0, 0, kWidth, kHeight);
    auto* top = panel(ui.dashboard, 0, 0, kWidth, kHeaderHeight, kCrust);
    label(top, "Camera monitor", lv_font_montserrat_14, kText, 13, 5);
    label(top, "LOCAL PREVIEW", lv_font_montserrat_12, kMuted, 13, 23);
    ui.wifi = label(top, "", lv_font_montserrat_12, kMuted, 189, 14, 118, LV_TEXT_ALIGN_RIGHT);

    for (auto& page : ui.pages) page = panel(ui.dashboard, 0, kHeaderHeight, kWidth, kActionsY - kHeaderHeight);
    auto* home = ui.pages[Home];
    cameraMark(home);
    ui.hero = label(home, "", monitor_font_28, kPeach, 88, 17, 225);
    lv_label_set_long_mode(ui.hero, LV_LABEL_LONG_CLIP);
    ui.homeDetail = label(home, "", lv_font_montserrat_12, kMuted, 88, 52);
    label(home, "60s sessions", lv_font_montserrat_12, kMuted, 15, 80);
    ui.hint = label(home, "", lv_font_montserrat_12, kMuted, 154, 80, 151, LV_TEXT_ALIGN_RIGHT);
    ui.start = button(home, 14, 100, 292, kBlue, 0x74a8fa, action);
    lv_obj_set_style_radius(ui.start, 6, 0);
    lv_obj_set_style_bg_color(ui.start, lv_color_hex(kSurface), LV_STATE_DISABLED);
    auto* startLabel = panel(ui.start, 0, 0, 116, 20, kBase, LV_OPA_TRANSP);
    lv_obj_center(startLabel);
    ui.play = label(startLabel, LV_SYMBOL_PLAY, lv_font_montserrat_12, kCrust, 0, 3);
    ui.startText = label(startLabel, "Start preview", lv_font_montserrat_14, kCrust, 19, 1);

    auto* status = ui.pages[Status];
    const char* keys[] = {"Wi-Fi", "Configuration", "Preview", "Idle backlight"};
    for (int i = 0; i < 4; ++i) {
        const auto y = static_cast<lv_coord_t>(19 + 32 * i);
        label(status, keys[i], lv_font_montserrat_12, kText, 15, y);
        ui.statusValues[i] = label(status, "", lv_font_montserrat_12, kBlue, 122, y, 183, LV_TEXT_ALIGN_RIGHT);
        if (i != 3) panel(status, 15, y + 22, 290, 1, kSurface);
    }

    auto* setup = ui.pages[Setup];
    label(setup, "USB setup", lv_font_montserrat_16, kPeach, 15, 14);
    label(setup, "1. Connect the USB cable", lv_font_montserrat_12, kText, 15, 44);
    label(setup, "2. Send your setup JSON", lv_font_montserrat_12, kText, 15, 65);
    label(setup, "3. Auto-restart; press BOOT", lv_font_montserrat_12, kText, 15, 86);
    label(setup, "115200 baud | configure while stopped", lv_font_montserrat_12, kMuted, 15, 116);

    auto* nav = panel(ui.dashboard, 0, kActionsY, kWidth, kTouchHeight, kCrust);
    const char* names[] = {"Home", "Status", "Setup"};
    for (int i = 0; i < PageCount; ++i) {
        ui.nav[i] = button(nav, kNavX[i], 0, kNavW[i], kCrust, kSurface, navigate);
        ui.navLabels[i] = label(ui.nav[i], names[i], lv_font_montserrat_14);
        lv_obj_center(ui.navLabels[i]);
    }
    ui.underline = panel(nav, 0, kTouchHeight - 3, kNavW[0], 3, kBlue);
    selectPage(Home);
}

void monitor(lv_obj_t* screen) {
    ui.monitor = panel(screen, 0, 0, kWidth, kHeight);
    ui.image = lv_img_create(ui.monitor);
    lv_obj_remove_style_all(ui.image);
    lv_obj_set_pos(ui.image, 0, 0);
    lv_obj_set_size(ui.image, kWidth, kHeight);
    ui.picture.header.cf = LV_IMG_CF_TRUE_COLOR;
    ui.picture.header.w = kWidth;
    ui.picture.header.h = kHeight;
    ui.picture.data_size = kWidth * kHeight * sizeof(uint16_t);
    visible(ui.image, false);

    auto* top = panel(ui.monitor, 0, 0, kWidth, kLiveHeaderHeight, kCrust, LV_OPA_90);
    ui.stateDot = panel(top, 10, 10, 6, 6, kYellow);
    lv_obj_set_style_radius(ui.stateDot, LV_RADIUS_CIRCLE, 0);
    ui.mode = label(top, "", lv_font_montserrat_14, kYellow, 22, 5);
    ui.remaining = label(top, "", lv_font_montserrat_14, kPeach, 185, 5, 125, LV_TEXT_ALIGN_RIGHT);

    auto* metadata = panel(ui.monitor, 0, kMetadataY, kWidth, kMetadataHeight, kCrust, LV_OPA_80);
    ui.frame = label(metadata, "", lv_font_montserrat_12, kText, 8, 2, 185);
    ui.age = label(metadata, "", lv_font_montserrat_12, kText, 193, 2, 119, LV_TEXT_ALIGN_RIGHT);
    lv_label_set_long_mode(ui.frame, LV_LABEL_LONG_CLIP);
    lv_label_set_long_mode(ui.age, LV_LABEL_LONG_CLIP);

    auto* actions = panel(ui.monitor, 0, kActionsY, kWidth, kTouchHeight, kCrust);
    ui.extend = button(actions, 0, 0, 192, kCrust, kSurface, action);
    lv_obj_center(label(ui.extend, "Continue 60s", lv_font_montserrat_14, kPeach));
    ui.stop = button(actions, 192, 0, 128, kCrust, kSurface, action);
    lv_obj_center(label(ui.stop, "Close", lv_font_montserrat_14, kRed));
    panel(actions, 192, 0, 1, kTouchHeight, kSurface);

    ui.notice = panel(ui.monitor, 27, 0, 266, 58, kCrust, kNoticeOpacity);
    lv_obj_set_style_radius(ui.notice, 7, 0);
    ui.noticeTitle = label(ui.notice, "", lv_font_montserrat_16, kYellow, 10, 9, kNoticeTextWidth, LV_TEXT_ALIGN_CENTER);
    ui.noticePicture = label(ui.notice, "", lv_font_montserrat_12, kText, 10, 33, kNoticeTextWidth, LV_TEXT_ALIGN_CENTER);
    ui.noticeDetail = label(ui.notice, "", lv_font_montserrat_12, kMuted, 10, kErrorTextY, kNoticeTextWidth, LV_TEXT_ALIGN_CENTER);
}

void showDashboard(const PreviewSnapshot& state) {
    const bool configured = state.configured;
    const bool online = configured && state.wifi;
    literal(ui.wifi, online ? LV_SYMBOL_WIFI "  Wi-Fi" : LV_SYMBOL_WIFI "  Wi-Fi off");
    textColor(ui.wifi, online ? kGreen : kMuted);
    literal(ui.hero, !configured ? "Setup required" : online ? "Ready" : "Wi-Fi offline");
    literal(ui.homeDetail, !configured ? "Configure via USB serial" : online ? "Preview is off" : "Waiting for Wi-Fi");
    literal(ui.hint, configured ? "BOOT to start" : "See Setup");
    const bool disabled = lv_obj_has_state(ui.start, LV_STATE_DISABLED);
    if (!configured && !disabled) lv_obj_add_state(ui.start, LV_STATE_DISABLED);
    else if (configured && disabled) lv_obj_clear_state(ui.start, LV_STATE_DISABLED);
    textColor(ui.startText, configured ? kCrust : kMuted);
    textColor(ui.play, configured ? kCrust : kMuted);

    literal(ui.statusValues[0], !configured ? "Not configured" : online ? "Connected" : "Disconnected");
    literal(ui.statusValues[1], configured ? "Ready" : "Setup required");
    literal(ui.statusValues[2], "Stopped");
    literal(ui.statusValues[3], configured ? "Off after 3s" : "Stays on for setup");
}

struct Mode {
    const char* name;
    uint32_t color;
    const char* title;
    bool showError = false;
};

Mode previewMode(const PreviewSnapshot& state, bool shown) {
    if (!state.wifi) return {shown ? "PAUSED" : "CONNECTING", kYellow, "Wi-Fi reconnecting"};
    if (std::strcmp(state.state, "paused_for_event") == 0) return {"PAUSED", kYellow, "Capturing event"};
    if (std::strcmp(state.state, "paused_for_enrollment") == 0) return {"PAUSED", kYellow, "Member enrollment"};
    if (std::strcmp(state.state, "paused_for_identity") == 0) return {"PAUSED", kYellow, "Identity processing"};
    if (std::strcmp(state.state, "error") == 0) return {"ERROR", kRed, "Camera error", true};
    if (*state.error) return {"ERROR", kRed, "Reconnecting", true};
    if (!shown) return {"CONNECTING", kYellow, "Connecting to camera"};
    if (state.stale) return {"PAUSED", kYellow, "Waiting for a fresh frame"};
    return {"LIVE", kGreen, nullptr};
}

void showMonitor(const PreviewSnapshot& state, bool haveImage) {
    const bool shown = haveImage && ui.picture.data;
    visible(ui.image, shown);
    const auto mode = previewMode(state, shown);
    literal(ui.mode, mode.name);
    textColor(ui.mode, mode.color);
    background(ui.stateDot, mode.color);
    char text[32];
    const uint32_t seconds = state.remainingMs / 1000 + (state.remainingMs % 1000 != 0);
    std::snprintf(text, sizeof text, "%lus left", static_cast<unsigned long>(seconds));
    copyText(ui.remaining, text);
    if (shown) {
        std::snprintf(text, sizeof text, "Frame %llu", static_cast<unsigned long long>(state.sequence));
        copyText(ui.frame, text);
        std::snprintf(text, sizeof text, "Age %lums", static_cast<unsigned long>(state.frameAgeMs));
        copyText(ui.age, text);
    } else {
        literal(ui.frame, "Waiting for first frame");
        literal(ui.age, "");
    }
    visible(ui.notice, mode.title != nullptr);
    if (!mode.title) return;
    literal(ui.noticeTitle, mode.title);
    textColor(ui.noticeTitle, mode.color);
    literal(ui.noticePicture, shown ? "Last frame shown." : "No picture yet.");
    const bool detail = mode.showError && *state.error;
    visible(ui.noticeDetail, detail);
    if (detail) {
        char message[sizeof state.error] = {};
        for (size_t i = 0; i + 1 < sizeof message && state.error[i]; ++i) {
            const auto byte = static_cast<unsigned char>(state.error[i]);
            message[i] = byte < 32 ? ' ' : byte > 126 ? '?' : static_cast<char>(byte);
        }
        if (copyText(ui.noticeDetail, message)) {
            // Match LVGL's word wrap, then cap the detail inside the notice.
            lv_point_t size{};
            lv_txt_get_size(&size, message, &lv_font_montserrat_12, 0, 0, kNoticeTextWidth, LV_TEXT_FLAG_NONE);
            ui.noticeDetailHeight = size.y < kMaxErrorTextHeight ? size.y : kMaxErrorTextHeight;
            lv_obj_set_height(ui.noticeDetail, ui.noticeDetailHeight);
        }
    }
    const lv_coord_t noticeHeight = detail ? kErrorTextY + ui.noticeDetailHeight + 9 : 58;
    lv_obj_set_height(ui.notice, noticeHeight);
    lv_obj_set_y(ui.notice, (kLiveHeaderHeight + kMetadataY - noticeHeight) / 2);
}
}

void monitorViewBegin(UiActionHandler handler) {
    ui = {};
    ui.handler = handler;
    auto* screen = lv_scr_act();
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    dashboard(screen);
    monitor(screen);
    monitorViewUpdate(PreviewSnapshot{}, false);
}

void monitorViewUpdate(const PreviewSnapshot& state, bool haveImage) {
    if (ui.active && !state.active) selectPage(Home);
    ui.active = state.active;
    ui.configured = state.configured;
    visible(ui.dashboard, !state.active);
    visible(ui.monitor, state.active);
    if (state.active) showMonitor(state, haveImage);
    else {
        visible(ui.image, false);
        showDashboard(state);
    }
}

void monitorViewSetFrame(const uint16_t* pixels) {
    if (!pixels) {
        monitorViewClearFrame();
        return;
    }
    lv_img_cache_invalidate_src(&ui.picture);
    ui.picture.data = reinterpret_cast<const uint8_t*>(pixels);
    if (lv_img_get_src(ui.image) != &ui.picture) lv_img_set_src(ui.image, &ui.picture);
    else lv_obj_invalidate(ui.image);
}

void monitorViewClearFrame() {
    visible(ui.image, false);
    if (ui.picture.data) lv_img_cache_invalidate_src(&ui.picture);
    ui.picture.data = nullptr;
}
}
