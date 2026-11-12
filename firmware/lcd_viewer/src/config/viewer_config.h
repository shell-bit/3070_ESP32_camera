#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lcd {

struct ViewerConfig {
    char ssid[33]{};
    char password[65]{};
    char host[128]{};
    char token[129]{};
    uint16_t controlPort = 80;
    uint16_t streamPort = 81;

    bool wifiReady() const;
    bool cameraReady() const;
    bool ready() const { return wifiReady() && cameraReady(); }
};

enum class ConfigError {
    None,
    Ssid,
    Password,
    Host,
    Token,
    TokenTooShort,
    ControlPort,
    StreamPort,
    StorageUnavailable,
    BackupWrite,
    BackupReadback,
    PrimaryWrite,
    PrimaryReadback,
    Inactive
};

const char* configErrorText(ConfigError error);

namespace config_detail {
inline bool reject(ConfigError& error, ConfigError reason) {
    error = reason;
    return false;
}

template <std::size_t N>
bool terminated(const char (&value)[N]) {
    return std::memchr(value, 0, N) != nullptr;
}

inline bool allowedCharacters(const char* value, const char* punctuation) {
    for (; *value; ++value) {
        const char c = *value;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) continue;
        if (!std::strchr(punctuation, c)) return false;
    }
    return true;
}

inline bool validatePorts(const ViewerConfig& config, ConfigError& error) {
    if (!config.controlPort) return reject(error, ConfigError::ControlPort);
    if (!config.streamPort) return reject(error, ConfigError::StreamPort);
    error = ConfigError::None;
    return true;
}
}

inline bool validateWifiConfig(const ViewerConfig& config, ConfigError& error) {
    using namespace config_detail;
    if (!config.ssid[0] || !terminated(config.ssid)) return reject(error, ConfigError::Ssid);
    if (!terminated(config.password)) return reject(error, ConfigError::Password);
    error = ConfigError::None;
    return true;
}

inline bool validateCameraConfig(const ViewerConfig& config, ConfigError& error) {
    using namespace config_detail;
    if (!config.host[0] || !terminated(config.host) || !allowedCharacters(config.host, ".-")) {
        return reject(error, ConfigError::Host);
    }
    if (!config.token[0] || !terminated(config.token) || !allowedCharacters(config.token, "-._~+/=")) {
        return reject(error, ConfigError::Token);
    }
    return validatePorts(config, error);
}

inline bool validateConfig(const ViewerConfig& config, ConfigError& error) {
    const bool wifiPresent = config.ssid[0] || config.password[0];
    const bool cameraPresent = config.host[0] || config.token[0];
    if (!wifiPresent && !cameraPresent) return config_detail::reject(error, ConfigError::Ssid);
    if (wifiPresent && !validateWifiConfig(config, error)) return false;
    if (cameraPresent) return validateCameraConfig(config, error);
    return config_detail::validatePorts(config, error);
}

inline bool ViewerConfig::wifiReady() const {
    ConfigError error;
    return validateWifiConfig(*this, error);
}

inline bool ViewerConfig::cameraReady() const {
    ConfigError error;
    return validateCameraConfig(*this, error);
}

bool loadConfig(ViewerConfig& config);
bool saveConfig(const ViewerConfig& config, ConfigError& error);

// JSON provisioning over local USB serial while idle. Never echoes secrets.
bool pollConfigSerial();
void discardConfigSerialInput();
void restartAfterConfigSave();

}
