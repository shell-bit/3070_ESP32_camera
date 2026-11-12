#include "setup_view.h"
#include "../board/board_port.h"
#include "../config/setup_controller.h"
#include "../preview/preview_client.h"
#include <cstdio>
#include <cstring>

namespace lcd {
namespace {

constexpr int kMargin = 12, kContent = kWidth - kMargin * 2;
constexpr int kFooterY = 196, kButtonHeight = 44;
constexpr uint32_t kBase = 0x1e1e2e, kCrust = 0x11111b, kSurface = 0x313244;
constexpr uint32_t kText = 0xcdd6f4, kMuted = 0xa6adc8, kBlue = 0x89b4fa;
constexpr uint32_t kPeach = 0xfab387, kGreen = 0xa6e3a1, kRed = 0xf38ba8;
constexpr uint32_t kReadInterval = 100, kUsbIdle = 120000;
constexpr char kAscii[] = " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~";
constexpr char kHostChars[] = "0123456789.ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-";
constexpr char kPortChars[] = "0123456789";
constexpr char kTokenChars[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-._~+/=";

enum class Page { Menu, Wifi, Scan, Password, Camera, Token, Advanced, Info, Usb, Editor, Discard };
enum class LeaveDestination { Menu, Home, Usb };
enum class Action {
    Wifi, Scan, Refresh, ManualSsid, Password, KeepPassword, ReplacePassword, OpenNetwork,
    Camera, Host, Token, KeepToken, ReplaceToken, Advanced, ControlPort, StreamPort,
    Info, Usb, Save, Start, Back, Discard, Stay, UseValue, ToggleHostLayout
};

struct View {
    SetupController edit;
    SetupExitHandler exit = nullptr;
    Page page = Page::Menu, editorParent = Page::Menu, discardFrom = Page::Menu;
    SetupField field = SetupField::Ssid;
    LeaveDestination discardDestination = LeaveDestination::Home;
    bool configured = false, scanBusy = false, requestRejected = false;
    bool restarting = false;
    uint32_t lastInput = 0, lastRead = 0, operation = 0, operationCounter = 0;
    ScanState shownScan = ScanState::Idle;
    SetupNetworkSnapshot network;
    ClockSnapshot clock;
    lv_obj_t* root = nullptr;
    lv_obj_t* message = nullptr;
    lv_obj_t* input = nullptr;
    lv_obj_t* keyboard = nullptr;
    lv_obj_t* list = nullptr;
    lv_obj_t* start = nullptr;
    lv_obj_t* info[6]{};
};

View ui;

void handle(Action action, uint32_t now);
void show(Page page);

void touch(uint32_t now) {
    ui.edit.touch(now);
    ui.lastInput = now;
}

void interaction(lv_event_t*) { touch(millis()); }

void setText(lv_obj_t* label, const char* text) {
    if (std::strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

void setColor(lv_obj_t* object, uint32_t color) {
    const auto value = lv_color_hex(color);
    if (lv_obj_get_style_text_color(object, LV_PART_MAIN).full != value.full) {
        lv_obj_set_style_text_color(object, value, 0);
    }
}

lv_obj_t* panel(lv_obj_t* parent, int x, int y, int width, int height, uint32_t color) {
    auto* object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return object;
}

lv_obj_t* label(lv_obj_t* parent, const char* text, int x, int y, int width = LV_SIZE_CONTENT,
                uint32_t color = kText, const lv_font_t& font = lv_font_montserrat_12) {
    auto* object = lv_label_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_width(object, width);
    lv_obj_set_style_text_font(object, &font, 0);
    lv_obj_set_style_text_color(object, lv_color_hex(color), 0);
    lv_label_set_text_static(object, text);
    return object;
}

lv_obj_t* caption(const char* text, int y, uint32_t color = kMuted) {
    return label(ui.root, text, kMargin, y, kContent, color);
}

void title(const char* name, const char* subtitle = "") {
    panel(ui.root, 0, 0, kWidth, 42, kCrust);
    label(ui.root, name, kMargin, 5, kContent, kPeach, lv_font_montserrat_16);
    ui.message = caption(subtitle, 25);
    lv_label_set_long_mode(ui.message, LV_LABEL_LONG_CLIP);
    lv_obj_set_height(ui.message, 16);
}

void clicked(lv_event_t* event) {
    handle(static_cast<Action>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event))), millis());
}

lv_obj_t* button(lv_obj_t* parent, int x, int y, int width, bool primary = false) {
    auto* object = lv_btn_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, kButtonHeight);
    lv_obj_set_style_radius(object, 6, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(primary ? kBlue : kSurface), 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(primary ? 0x74a8fa : 0x45475a), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(object, lv_color_hex(kSurface), LV_STATE_DISABLED);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(object, LV_OBJ_FLAG_EVENT_BUBBLE);
    return object;
}

lv_obj_t* actionButton(const char* text, Action action, int x, int y, int width, bool primary = false) {
    auto* object = button(ui.root, x, y, width, primary);
    lv_obj_center(label(object, text, 0, 0, LV_SIZE_CONTENT, primary ? kCrust : kText, lv_font_montserrat_14));
    lv_obj_add_event_cb(object, clicked, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<uintptr_t>(action)));
    return object;
}

lv_obj_t* footer(const char* left, Action leftAction, const char* right = nullptr, Action rightAction = Action::Back) {
    panel(ui.root, 0, kFooterY, kWidth, kButtonHeight, kCrust);
    actionButton(left, leftAction, 0, kFooterY, right ? 114 : kWidth);
    return right ? actionButton(right, rightAction, 116, kFooterY, kWidth - 116, true) : nullptr;
}

bool scanInProgress(ScanState state) {
    return state == ScanState::Queued || state == ScanState::Scanning || state == ScanState::Cancelling;
}

void readNetwork(uint32_t now) {
    ui.lastRead = now;
    ui.network = previewSetupSnapshot();
    const bool wasBusy = ui.scanBusy;
    ui.scanBusy = scanInProgress(ui.network.scanState);
    if (wasBusy && !ui.scanBusy && !ui.edit.active()) discardConfigSerialInput();
}

void cancelScan() {
    if (!ui.operation) return;
    previewCancelScan(ui.operation);
    ui.operation = 0;
    readNetwork(millis());
}

void clearInput() {
    if (!ui.input) return;
    lv_keyboard_set_textarea(ui.keyboard, nullptr);
    // LVGL owns this writable text buffer. Clear it before freeing the widget.
    const char* text = lv_textarea_get_text(ui.input);
    auto* bytes = const_cast<volatile char*>(text);
    const auto length = std::strlen(text);
    for (size_t i = 0; i < length; ++i) bytes[i] = 0;
    lv_textarea_set_text(ui.input, "");
    ui.input = ui.keyboard = nullptr;
}

void close(SetupExit exit = SetupExit::Home) {
    cancelScan();
    clearInput();
    ui.edit.discard();
    discardConfigSerialInput();
    lv_obj_del(ui.root);
    ui.root = nullptr;
    ui.exit(exit);
}

void releaseToUsb() {
    cancelScan();
    ui.edit.discard();
    discardConfigSerialInput();
    show(Page::Usb);
}

void finishLeaving(LeaveDestination destination, uint32_t now) {
    switch (destination) {
        case LeaveDestination::Menu: ui.edit.discardSection(now); show(Page::Menu); break;
        case LeaveDestination::Home: close(); break;
        case LeaveDestination::Usb: releaseToUsb(); break;
    }
}

void leave(LeaveDestination destination, uint32_t now) {
    if (ui.edit.dirty()) {
        ui.discardDestination = destination;
        ui.discardFrom = ui.page;
        show(Page::Discard);
    } else {
        finishLeaving(destination, now);
    }
}

void escapeSsid(const char* ssid, char (&text)[129]) {
    constexpr char hex[] = "0123456789ABCDEF";
    size_t out = 0;
    for (size_t i = 0; i < 32 && ssid[i]; ++i) {
        const auto byte = static_cast<unsigned char>(ssid[i]);
        if (byte >= 32 && byte <= 126 && byte != '\\') {
            text[out++] = static_cast<char>(byte);
        } else {
            text[out++] = '\\';
            text[out++] = 'x';
            text[out++] = hex[byte >> 4];
            text[out++] = hex[byte & 15];
        }
    }
    text[out] = '\0';
}

void fitScanName(char (&name)[129], lv_coord_t width) {
    const auto* font = &lv_font_montserrat_12;
    const auto length = std::strlen(name);
    if (lv_txt_get_width(name, length, font, 0, LV_TEXT_FLAG_NONE) <= width) return;
    width -= lv_txt_get_width("...", 3, font, 0, LV_TEXT_FLAG_NONE);
    size_t end = 0;
    while (end < length) {
        const size_t next = end + (name[end] == '\\' ? 4 : 1);
        if (lv_txt_get_width(name, next, font, 0, LV_TEXT_FLAG_NONE) > width) break;
        end = next;
    }
    std::strcpy(name + end, "...");
}

void showMenu() {
    title("Setup", "Wi-Fi and camera settings");
    actionButton("Wi-Fi", Action::Wifi, 12, 51, 144);
    actionButton("Camera", Action::Camera, 164, 51, 144);
    actionButton("Connection info", Action::Info, 12, 103, 144);
    actionButton("USB setup", Action::Usb, 164, 103, 144);
    footer("Back", Action::Back);
}

void showWifi() {
    title("Wi-Fi", "Save Wi-Fi settings and restart the LCD");
    caption("SSID", 55);
    char name[129];
    escapeSsid(ui.edit.draft().ssid, name);
    auto* value = label(ui.root, "", 66, 53, 242, kText, lv_font_montserrat_14);
    lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
    lv_obj_set_height(value, 18);
    setText(value, *name ? name : "Not set");
    actionButton("Manual SSID", Action::ManualSsid, 12, 85, 144);
    actionButton("Password", Action::Password, 164, 85, 144);
    actionButton("Scan", Action::Scan, 12, 137, 144);
    label(ui.root, "Scan preserves non-ASCII names.", 164, 145, 144, kMuted);
    footer("Back", Action::Back, "Save & restart", Action::Save);
}

void selectNetwork(lv_event_t* event) {
    const auto index = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    if (!ui.operation || ui.network.operationId != ui.operation || ui.network.scanState != ScanState::Ready ||
        index >= ui.network.count || index >= sizeof ui.network.networks / sizeof ui.network.networks[0]) return;
    const uint32_t now = millis();
    touch(now);
    if (ui.edit.setText(SetupField::Ssid, ui.network.networks[index].ssid, now)) show(Page::Wifi);
}

void drawScan() {
    if (!ui.operation) {
        setText(ui.message, ui.requestRejected ? "Wi-Fi is busy. Try Refresh." : "Tap Refresh to scan");
        return;
    }
    if (ui.network.operationId != ui.operation) {
        if (ui.shownScan == ScanState::Ready) lv_obj_clean(ui.list);
        ui.shownScan = ScanState::Idle;
        setText(ui.message, "Waiting for scan");
        return;
    }
    const auto state = ui.network.scanState;
    if (ui.shownScan == state) return;
    ui.shownScan = state;
    lv_obj_clean(ui.list);
    switch (state) {
        case ScanState::Idle: setText(ui.message, "Tap Refresh to scan"); break;
        case ScanState::Queued: setText(ui.message, "Waiting to scan"); break;
        case ScanState::Scanning: setText(ui.message, "Scanning for networks..."); break;
        case ScanState::Cancelling: setText(ui.message, "Finishing previous scan..."); break;
        case ScanState::Failed:
            setText(ui.message, *ui.network.error ? ui.network.error : "Scan failed. Try Refresh.");
            break;
        case ScanState::Ready:
            setText(ui.message, ui.network.count ? "Select a network" : "No networks found");
            for (size_t i = 0; i < ui.network.count && i < sizeof ui.network.networks / sizeof ui.network.networks[0]; ++i) {
                const auto& network = ui.network.networks[i];
                auto* row = button(ui.list, 0, i * 48, kContent - 5);
                char name[129];
                escapeSsid(network.ssid, name);
                constexpr int textWidth = kContent - 23;
                fitScanName(name, textWidth);
                char description[180];
                std::snprintf(description, sizeof description, "%s\n%s  %ld dBm  %s", name,
                              network.security ? "Secured" : "Open", static_cast<long>(network.rssi), network.suffix);
                auto* details = label(row, "", 9, 6, textWidth);
                lv_obj_set_style_text_line_space(details, 4, 0);
                lv_label_set_long_mode(details, LV_LABEL_LONG_CLIP);
                setText(details, description);
                lv_obj_add_event_cb(row, selectNetwork, LV_EVENT_CLICKED, reinterpret_cast<void*>(i));
            }
            break;
    }
}

void showScan() {
    title("Nearby Wi-Fi");
    ui.list = panel(ui.root, kMargin, 45, kContent, 146, kBase);
    lv_obj_add_flag(ui.list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_scroll_dir(ui.list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ui.list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(ui.list, lv_color_hex(kMuted), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(ui.list, LV_OPA_80, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(ui.list, 3, LV_PART_SCROLLBAR);
    footer("Back", Action::Back, "Refresh", Action::Refresh);
    drawScan();
}

void requestScan(uint32_t now) {
    readNetwork(now);
    if (ui.scanBusy) return;
    cancelScan();
    if (++ui.operationCounter == 0) ++ui.operationCounter;
    ui.requestRejected = !previewRequestScan(ui.operationCounter);
    ui.operation = ui.requestRejected ? 0 : ui.operationCounter;
    ui.shownScan = ScanState::Idle;
    lv_obj_clean(ui.list);
    readNetwork(now);
    drawScan();
}

void showPassword() {
    title("Wi-Fi password", *ui.edit.draft().password ? "A password is set in this draft" : "No password set in this draft");
    actionButton("Keep password", Action::KeepPassword, 12, 48, kContent);
    actionButton("Replace password", Action::ReplacePassword, 12, 96, kContent);
    actionButton("Use open network", Action::OpenNetwork, 12, 144, kContent);
    footer("Back", Action::Back);
}

void updateShortcut() {
    const bool enabled = ui.configured && !ui.edit.dirty() && !ui.scanBusy;
    const bool disabled = lv_obj_has_state(ui.start, LV_STATE_DISABLED);
    if (enabled == disabled) {
        if (enabled) lv_obj_clear_state(ui.start, LV_STATE_DISABLED);
        else lv_obj_add_state(ui.start, LV_STATE_DISABLED);
    }
    setColor(lv_obj_get_child(ui.start, 0), enabled ? kText : kMuted);
}

void showCamera() {
    title("Camera", "Save camera settings and restart the LCD");
    caption("Address", 55);
    auto* host = label(ui.root, "", 82, 53, 226, kText, lv_font_montserrat_14);
    lv_label_set_long_mode(host, LV_LABEL_LONG_DOT);
    lv_obj_set_height(host, 18);
    setText(host, *ui.edit.draft().host ? ui.edit.draft().host : "Not set");
    actionButton("Host", Action::Host, 12, 85, 144);
    actionButton("Token", Action::Token, 164, 85, 144);
    actionButton("Advanced", Action::Advanced, 12, 137, 144);
    ui.start = actionButton("Start preview", Action::Start, 164, 137, 144);
    footer("Back", Action::Back, "Save & restart", Action::Save);
    updateShortcut();
}

void showToken() {
    title("Camera token", *ui.edit.draft().token ? "A token is set in this draft" : "No token set in this draft");
    actionButton("Keep token", Action::KeepToken, 12, 51, kContent);
    actionButton("Replace token", Action::ReplaceToken, 12, 103, kContent);
    caption("Use the camera's pairing token (24 or more characters).", 159);
    footer("Back", Action::Back);
}

void showAdvanced() {
    title("Camera ports", "Default ports are 80 and 81");
    actionButton("Control port", Action::ControlPort, 12, 56, 188);
    actionButton("Stream port", Action::StreamPort, 12, 108, 188);
    char port[6];
    std::snprintf(port, sizeof port, "%u", static_cast<unsigned>(ui.edit.draft().controlPort));
    setText(label(ui.root, "", 222, 69, 86, kText, lv_font_montserrat_14), port);
    std::snprintf(port, sizeof port, "%u", static_cast<unsigned>(ui.edit.draft().streamPort));
    setText(label(ui.root, "", 222, 121, 86, kText, lv_font_montserrat_14), port);
    footer("Back", Action::Back);
}

void drawInfo() {
    setText(ui.info[0], ui.network.connected ? "Connected" : "Disconnected");
    setColor(ui.info[0], ui.network.connected ? kGreen : kMuted);
    char name[129];
    escapeSsid(ui.network.ssid, name);
    setText(ui.info[1], ui.network.connected && *name ? name : "-");
    setText(ui.info[2], ui.network.connected && *ui.network.ip ? ui.network.ip : "-");
    char signal[24];
    std::snprintf(signal, sizeof signal, "%ld dBm", static_cast<long>(ui.network.rssi));
    setText(ui.info[3], ui.network.connected ? signal : "-");
    char time[28];
    std::snprintf(time, sizeof time, "%s  %s", ui.clock.date, ui.clock.time);
    setText(ui.info[4], ui.clock.valid ? time : "--:--:--");
    setText(ui.info[5], !ui.clock.valid ? "Time not synchronized" : ui.clock.online ? "Time synchronized" : "Time offline holdover");
}

void showInfo() {
    title("Connection info", "LCD network and clock");
    const char* names[] = {"Wi-Fi", "SSID", "LCD IP", "Signal", "UTC+8", "Clock"};
    for (size_t i = 0; i < sizeof ui.info / sizeof ui.info[0]; ++i) {
        const int y = 49 + i * 23;
        caption(names[i], y);
        ui.info[i] = label(ui.root, "", 99, y, 209);
        lv_label_set_long_mode(ui.info[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_height(ui.info[i], 18);
    }
    footer("Back", Action::Back);
    readNetwork(millis());
    drawInfo();
}

void showUsb() {
    title("USB setup", "Serial provisioning while preview is stopped");
    caption("Connect USB at 115200 baud.", 52, kText);
    caption("Send one complete JSON line with wifi_ssid, wifi_password, camera_host and token.", 78, kText);
    caption("control_port and stream_port are optional.", 122);
    caption("Use empty strings for both fields of an unused group.", 144);
    caption("The LCD restarts after a successful save.", 176);
    footer("Back", Action::Back);
}

struct FieldSpec {
    const char* title;
    const char* hint;
    const char* accepted;
    const char* error;
    uint16_t limit;
    bool secret = false;
    bool numeric = false;
};

constexpr FieldSpec kFields[] = {
    {"Manual SSID", "Network name (ASCII, up to 32 bytes)", kAscii, "Enter a network name", 32},
    {"Replace password", "Wi-Fi password (up to 64 bytes)", kAscii, "Enter a password or choose Open network", 64, true},
    {"Camera host", "Hostname or IPv4, e.g. 192.168.1.50", kHostChars, "Enter a hostname or IPv4 address", 127},
    {"Replace token", "Camera pairing token (24 to 128 characters)", kTokenChars, "Enter a pairing token", 128, true},
    {"Control port", "Enter a port from 1 to 65535", kPortChars, "Port must be 1 to 65535", 5, false, true},
    {"Stream port", "Enter a port from 1 to 65535", kPortChars, "Port must be 1 to 65535", 5, false, true},
};

const FieldSpec& fieldSpec() { return kFields[static_cast<unsigned>(ui.field)]; }

void keyboardDone(lv_event_t* event) {
    handle(lv_event_get_code(event) == LV_EVENT_CANCEL ? Action::Back : Action::UseValue, millis());
}

// Include all printable ASCII that the default letter layouts lack.
const char* kSymbols[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "abc", "+", "-", "/", "*", "=", "%", "!", "?", "#", "<", ">", "\n",
    "\\", "@", "$", "(", ")", "{", "}", "[", "]", ";", "\"", "'", "\n",
    "`", "~", "&", "^", "|", LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};

const lv_btnmatrix_ctrl_t kSymbolWidths[] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2,
    2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 3, 1, 2
};

void showEditor() {
    const auto& field = fieldSpec();
    title(field.title, field.hint);
    const bool host = ui.field == SetupField::Host;
    ui.input = lv_textarea_create(ui.root);
    lv_obj_set_pos(ui.input, 12, 43);
    lv_obj_set_size(ui.input, host ? 205 : kContent, 44);
    lv_obj_set_style_bg_color(ui.input, lv_color_hex(kCrust), 0);
    lv_obj_set_style_border_color(ui.input, lv_color_hex(kBlue), 0);
    lv_obj_set_style_text_color(ui.input, lv_color_hex(kText), 0);
    lv_obj_set_style_text_color(ui.input, lv_color_hex(kMuted), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_text_font(ui.input, &lv_font_montserrat_14, 0);
    lv_textarea_set_one_line(ui.input, true);
    lv_textarea_set_accepted_chars(ui.input, field.accepted);
    lv_textarea_set_max_length(ui.input, field.limit);
    lv_textarea_set_placeholder_text(ui.input, "Enter new value");
    if (field.secret) {
        lv_textarea_set_password_show_time(ui.input, 0);
        lv_textarea_set_password_mode(ui.input, true);
    }
    lv_obj_add_event_cb(ui.input, interaction, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_flag(ui.input, LV_OBJ_FLAG_EVENT_BUBBLE);

    ui.keyboard = lv_keyboard_create(ui.root);
    lv_obj_set_size(ui.keyboard, kWidth, 103);
    lv_obj_align(ui.keyboard, LV_ALIGN_TOP_LEFT, 0, 91);
    lv_obj_set_style_bg_color(ui.keyboard, lv_color_hex(kBase), 0);
    lv_obj_set_style_bg_color(ui.keyboard, lv_color_hex(kSurface), LV_PART_ITEMS);
    lv_obj_set_style_text_color(ui.keyboard, lv_color_hex(kText), LV_PART_ITEMS);
    lv_obj_set_style_text_font(ui.keyboard, &lv_font_montserrat_12, LV_PART_ITEMS);
    lv_obj_set_style_pad_all(ui.keyboard, 4, 0);
    lv_obj_set_style_pad_row(ui.keyboard, 3, 0);
    lv_obj_set_style_pad_column(ui.keyboard, 3, 0);
    lv_keyboard_set_map(ui.keyboard, LV_KEYBOARD_MODE_SPECIAL, kSymbols, kSymbolWidths);
    lv_keyboard_set_popovers(ui.keyboard, false);
    lv_keyboard_set_mode(ui.keyboard, field.numeric ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(ui.keyboard, ui.input);
    lv_obj_add_flag(ui.keyboard, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(ui.keyboard, keyboardDone, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(ui.keyboard, keyboardDone, LV_EVENT_CANCEL, nullptr);
    lv_obj_add_event_cb(ui.input, keyboardDone, LV_EVENT_READY, nullptr);
    if (host) actionButton("IPv4 / ABC", Action::ToggleHostLayout, 221, 43, 87);
    footer("Cancel", Action::Back, "Use value", Action::UseValue);
}

void showDiscard() {
    title("Discard changes?");
    const char* text = ui.discardFrom == Page::Wifi ? "Your Wi-Fi changes will be lost." :
                       ui.discardFrom == Page::Camera ? "Your camera changes will be lost." :
                       "Your unsaved changes will be lost.";
    caption(text, 64, kText);
    footer("Discard", Action::Discard, "Keep editing", Action::Stay);
}

void show(Page page) {
    if (ui.page == Page::Scan && page != Page::Scan) cancelScan();
    clearInput();
    lv_obj_clean(ui.root);
    ui.page = page;
    switch (page) {
        case Page::Menu: showMenu(); break;
        case Page::Wifi: showWifi(); break;
        case Page::Scan: showScan(); break;
        case Page::Password: showPassword(); break;
        case Page::Camera: showCamera(); break;
        case Page::Token: showToken(); break;
        case Page::Advanced: showAdvanced(); break;
        case Page::Info: showInfo(); break;
        case Page::Usb: showUsb(); break;
        case Page::Editor: showEditor(); break;
        case Page::Discard: showDiscard(); break;
    }
}

void edit(SetupField field) {
    ui.field = field;
    ui.editorParent = ui.page;
    show(Page::Editor);
}

void useValue(uint32_t now) {
    const char* value = lv_textarea_get_text(ui.input);
    if (*value && ui.edit.setText(ui.field, value, now)) {
        show(ui.editorParent);
    } else {
        setText(ui.message, fieldSpec().error);
        setColor(ui.message, kRed);
    }
}

void save() {
    if (ui.scanBusy) {
        setText(ui.message, "Wait for the Wi-Fi scan to finish");
        return;
    }
    ConfigError error;
    if (!ui.edit.save(error)) {
        setText(ui.message, configErrorText(error));
        setColor(ui.message, kRed);
        return;
    }
    ui.restarting = true;
    close();
    restartAfterConfigSave();
}

void handle(Action action, uint32_t now) {
    touch(now);
    switch (action) {
        case Action::Wifi:
            if (ui.edit.selectSection(SetupSection::Wifi, now)) show(Page::Wifi);
            break;
        case Action::Scan: show(Page::Scan); requestScan(now); break;
        case Action::Refresh: requestScan(now); break;
        case Action::ManualSsid: edit(SetupField::Ssid); break;
        case Action::Password: show(Page::Password); break;
        case Action::KeepPassword: ui.edit.keepSecret(SetupField::Password, now); show(Page::Wifi); break;
        case Action::ReplacePassword: edit(SetupField::Password); break;
        case Action::OpenNetwork: ui.edit.setOpenNetwork(now); show(Page::Wifi); break;
        case Action::Camera:
            if (ui.edit.selectSection(SetupSection::Camera, now)) show(Page::Camera);
            break;
        case Action::Host: edit(SetupField::Host); break;
        case Action::Token: show(Page::Token); break;
        case Action::KeepToken: ui.edit.keepSecret(SetupField::Token, now); show(Page::Camera); break;
        case Action::ReplaceToken: edit(SetupField::Token); break;
        case Action::Advanced: show(Page::Advanced); break;
        case Action::ControlPort: edit(SetupField::ControlPort); break;
        case Action::StreamPort: edit(SetupField::StreamPort); break;
        case Action::Info: show(Page::Info); break;
        case Action::Usb: leave(LeaveDestination::Usb, now); break;
        case Action::Save: save(); break;
        case Action::Start:
            if (ui.configured && !ui.edit.dirty() && !ui.scanBusy) close(SetupExit::StartPreview);
            break;
        case Action::Back: setupViewBack(now); break;
        case Action::Discard: finishLeaving(ui.discardDestination, now); break;
        case Action::Stay: show(ui.discardFrom); break;
        case Action::UseValue: useValue(now); break;
        case Action::ToggleHostLayout:
            lv_keyboard_set_mode(ui.keyboard, lv_keyboard_get_mode(ui.keyboard) == LV_KEYBOARD_MODE_NUMBER ?
                                 LV_KEYBOARD_MODE_TEXT_LOWER : LV_KEYBOARD_MODE_NUMBER);
            break;
    }
}

}

void setupViewBegin(SetupExitHandler handler) {
    ui = View{};
    ui.exit = handler;
}

void setupViewOpen(uint32_t now, bool configured) {
    if (ui.root || ui.restarting) return;
    auto config = previewConfig();
    ui.edit.begin(config, now);
    auto* bytes = reinterpret_cast<volatile unsigned char*>(&config);
    for (size_t i = 0; i < sizeof config; ++i) bytes[i] = 0;
    ui.configured = configured;
    touch(now);
    discardConfigSerialInput();
    ui.root = panel(lv_scr_act(), 0, 0, kWidth, kHeight, kBase);
    lv_obj_add_event_cb(ui.root, interaction, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(ui.root, interaction, LV_EVENT_PRESSING, nullptr);
    show(Page::Menu);
}

void setupViewLoop(uint32_t now, const ClockSnapshot& clock) {
    ui.clock = clock;
    if (ui.root && (ui.edit.timedOut(now) || (!ui.edit.active() && now - ui.lastInput >= kUsbIdle))) close();
    if (now - ui.lastRead < kReadInterval) return;
    if (ui.scanBusy || (ui.root && (ui.page == Page::Scan || ui.page == Page::Info))) readNetwork(now);
    if (!ui.root) return;
    if (ui.page == Page::Scan) drawScan();
    if (ui.page == Page::Info) drawInfo();
    if (ui.page == Page::Camera) updateShortcut();
}

void setupViewBack(uint32_t now) {
    if (!ui.root) return;
    touch(now);
    switch (ui.page) {
        case Page::Menu: leave(LeaveDestination::Home, now); break;
        case Page::Wifi:
        case Page::Camera: leave(LeaveDestination::Menu, now); break;
        case Page::Info: show(Page::Menu); break;
        case Page::Discard: show(ui.discardFrom); break;
        case Page::Password:
        case Page::Scan: show(Page::Wifi); break;
        case Page::Token:
        case Page::Advanced: show(Page::Camera); break;
        case Page::Editor: show(ui.editorParent); break;
        case Page::Usb: close(); break;
    }
}

bool setupViewVisible() { return ui.root != nullptr; }
bool setupViewOwnsDraft() { return ui.edit.active() || ui.restarting; }
bool setupViewScanBusy() { return ui.scanBusy; }

}
