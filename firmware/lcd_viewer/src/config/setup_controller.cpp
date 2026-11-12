#include "setup_controller.h"

#include <cstddef>
#include <cstring>

namespace lcd {
namespace {
constexpr uint32_t inactivityMs = 120000;
constexpr size_t minimumSetupToken = 24;

struct SaveCandidate {
    ViewerConfig value;

    ~SaveCandidate() {
        auto* byte = reinterpret_cast<volatile unsigned char*>(&value);
        for (size_t i = 0; i < sizeof(value); ++i) byte[i] = 0;
    }
};

SetupSection fieldSection(SetupField field) {
    switch (field) {
    case SetupField::Ssid:
    case SetupField::Password: return SetupSection::Wifi;
    case SetupField::Host:
    case SetupField::Token:
    case SetupField::ControlPort:
    case SetupField::StreamPort: return SetupSection::Camera;
    }
    return SetupSection::None;
}

template <size_t N>
bool replaceText(char (&destination)[N], const char* text, bool allowEmpty = true) {
    size_t length = 0;
    while (length < N && text[length]) ++length;
    if (length == N || (!allowEmpty && length == 0)) return false;
    std::memmove(destination, text, length);
    std::memset(destination + length, 0, N - length);
    return true;
}

template <size_t N>
bool changed(const char (&original)[N], const char (&draft)[N]) {
    return std::strncmp(original, draft, N) != 0;
}

bool replacePort(uint16_t& port, const char* text) {
    uint32_t value = 0;
    for (size_t i = 0; text[i]; ++i) {
        if (i == 5 || text[i] < '0' || text[i] > '9') return false;
        value = value * 10 + static_cast<uint32_t>(text[i] - '0');
    }
    if (value == 0 || value > 65535) return false;
    port = static_cast<uint16_t>(value);
    return true;
}
}

void SetupController::begin(const ViewerConfig& config, uint32_t now) {
    original_ = config;
    draft_ = config;
    lastInput_ = now;
    active_ = true;
    section_ = SetupSection::None;
}

void SetupController::discard() {
    original_ = {};
    draft_ = {};
    lastInput_ = 0;
    active_ = false;
    section_ = SetupSection::None;
}

bool SetupController::selectSection(SetupSection section, uint32_t now) {
    if (!active_) return false;
    if (section != section_) {
        if (dirty()) return false;
        draft_ = original_;
        section_ = section;
    }
    touch(now);
    return true;
}

void SetupController::discardSection(uint32_t now) {
    if (!active_) return;
    draft_ = original_;
    section_ = SetupSection::None;
    touch(now);
}

bool SetupController::networkDirty() const {
    return active_ && (changed(original_.ssid, draft_.ssid) ||
                       changed(original_.password, draft_.password));
}

bool SetupController::dirty() const {
    return active_ && (networkDirty() || changed(original_.host, draft_.host) ||
                       changed(original_.token, draft_.token) ||
                       original_.controlPort != draft_.controlPort ||
                       original_.streamPort != draft_.streamPort);
}

bool SetupController::setText(SetupField field, const char* text, uint32_t now) {
    if (!text || section_ == SetupSection::None || section_ != fieldSection(field)) return false;
    touch(now);
    switch (field) {
    case SetupField::Ssid: return replaceText(draft_.ssid, text);
    case SetupField::Password: return replaceText(draft_.password, text, false);
    case SetupField::Host: return replaceText(draft_.host, text);
    case SetupField::Token: return replaceText(draft_.token, text, false);
    case SetupField::ControlPort: return replacePort(draft_.controlPort, text);
    case SetupField::StreamPort: return replacePort(draft_.streamPort, text);
    }
    return false;
}

void SetupController::keepSecret(SetupField field, uint32_t now) {
    if (section_ == SetupSection::Wifi && field == SetupField::Password) {
        std::memcpy(draft_.password, original_.password, sizeof(draft_.password));
    } else if (section_ == SetupSection::Camera && field == SetupField::Token) {
        std::memcpy(draft_.token, original_.token, sizeof(draft_.token));
    } else {
        return;
    }
    touch(now);
}

void SetupController::setOpenNetwork(uint32_t now) {
    if (section_ != SetupSection::Wifi) return;
    touch(now);
    std::memset(draft_.password, 0, sizeof(draft_.password));
}

void SetupController::touch(uint32_t now) {
    if (active_) lastInput_ = now;
}

bool SetupController::timedOut(uint32_t now) const {
    return active_ && now - lastInput_ >= inactivityMs;
}

bool SetupController::prepareCandidate(ViewerConfig& candidate, ConfigError& error) const {
    if (section_ == SetupSection::None) {
        error = ConfigError::Inactive;
        return false;
    }

    candidate = original_;
    if (section_ == SetupSection::Wifi) {
        std::memcpy(candidate.ssid, draft_.ssid, sizeof(candidate.ssid));
        std::memcpy(candidate.password, draft_.password, sizeof(candidate.password));
        if (!validateWifiConfig(candidate, error)) return false;
    } else {
        std::memcpy(candidate.host, draft_.host, sizeof(candidate.host));
        std::memcpy(candidate.token, draft_.token, sizeof(candidate.token));
        candidate.controlPort = draft_.controlPort;
        candidate.streamPort = draft_.streamPort;
        if (!validateCameraConfig(candidate, error)) return false;
        if (std::strlen(candidate.token) < minimumSetupToken) {
            error = ConfigError::TokenTooShort;
            return false;
        }
    }
    return validateConfig(candidate, error);
}

bool SetupController::validate(ConfigError& error) const {
    SaveCandidate candidate;
    return prepareCandidate(candidate.value, error);
}

bool SetupController::save(ConfigError& error) {
    SaveCandidate candidate;
    if (!prepareCandidate(candidate.value, error) || !saveConfig(candidate.value, error)) return false;
    discard();
    return true;
}
}
