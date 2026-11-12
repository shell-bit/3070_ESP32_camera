#include "viewer_config.h"

#include <Arduino.h>
#include <Preferences.h>
#include <cJSON.h>
#include <cstddef>
#include <cstring>

namespace lcd {
namespace {
constexpr char storageNamespace[] = "lcd-preview";
constexpr char primaryKey[] = "config";
constexpr char previousKey[] = "config-prev";
constexpr size_t serialBudget = 512;

static_assert(sizeof(ViewerConfig) == 360);
static_assert(offsetof(ViewerConfig, controlPort) == 356);
static_assert(offsetof(ViewerConfig, streamPort) == 358);

// Only the setup/serial owner accesses NVS after boot. Hold the recovery
// record across retries when a primary write may have committed but failed verification.
bool protectPrevious = false;

struct SerialInput {
    char line[512]{};
    size_t used = 0;
    size_t pendingDiscard = 0;
    bool discardLine = false;
    bool overflow = false;
    bool hasNul = false;
};

SerialInput serialInput;

void clearBytes(void* data, size_t size) {
    auto* byte = static_cast<volatile unsigned char*>(data);
    while (size--) *byte++ = 0;
}

struct ConfigBuffer {
    ViewerConfig value;

    ~ConfigBuffer() { clearBytes(&value, sizeof(value)); }
};

using config_detail::reject;

bool readBlob(Preferences& prefs, const char* key, ViewerConfig& config) {
    ConfigError error;
    return prefs.getBytesLength(key) == sizeof(config) &&
           prefs.getBytes(key, &config, sizeof(config)) == sizeof(config) &&
           validateConfig(config, error);
}

bool checkedWrite(Preferences& prefs, const char* key, const ViewerConfig& config,
                  ConfigError writeError, ConfigError readbackError, ConfigError& error) {
    if (prefs.putBytes(key, &config, sizeof(config)) != sizeof(config)) return reject(error, writeError);
    ConfigBuffer readback;
    if (!readBlob(prefs, key, readback.value) ||
        std::memcmp(&config, &readback.value, sizeof(config)) != 0) {
        return reject(error, readbackError);
    }
    return true;
}

template <size_t N>
bool readString(const cJSON* root, const char* key, char (&destination)[N]) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(value) || !value->valuestring) return false;
    const size_t length = std::strlen(value->valuestring);
    if (length >= N) return false;
    std::memcpy(destination, value->valuestring, length + 1);
    return true;
}

bool readPort(const cJSON* root, const char* key, uint16_t& port) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!value) return true;
    if (!cJSON_IsNumber(value) || value->valuedouble < 1 || value->valuedouble > 65535 ||
        value->valuedouble != value->valueint) return false;
    port = static_cast<uint16_t>(value->valueint);
    return true;
}

bool hasInvalidUnicodeEscape(const char* line) {
    // cJSON can decode both zero and malformed hex to NUL in a C string.
    while ((line = std::strchr(line, '\\')) != nullptr) {
        ++line;
        if (*line == 'u') {
            for (size_t i = 1; i <= 4; ++i) {
                if (!line[i] || !std::strchr("0123456789abcdefABCDEF", line[i])) return true;
            }
            if (std::strncmp(line + 1, "0000", 4) == 0) return true;
            line += 4;
        }
        if (*line) ++line;
    }
    return false;
}

bool parseConfig(const char* line, ViewerConfig& config, ConfigError& error) {
    if (hasInvalidUnicodeEscape(line)) return false;
    auto* json = cJSON_ParseWithOpts(line, nullptr, true);
    bool parsed = false;
    if (cJSON_IsObject(json)) {
        if (!readString(json, "wifi_ssid", config.ssid)) error = ConfigError::Ssid;
        else if (!readString(json, "wifi_password", config.password)) error = ConfigError::Password;
        else if (!readString(json, "camera_host", config.host)) error = ConfigError::Host;
        else if (!readString(json, "token", config.token)) error = ConfigError::Token;
        else if (!readPort(json, "control_port", config.controlPort)) error = ConfigError::ControlPort;
        else if (!readPort(json, "stream_port", config.streamPort)) error = ConfigError::StreamPort;
        else parsed = true;

        for (auto* item = json->child; item; item = item->next) {
            if (cJSON_IsString(item) && item->valuestring) {
                clearBytes(item->valuestring, std::strlen(item->valuestring));
            }
        }
    }
    cJSON_Delete(json);
    return parsed;
}

void resetSerialLine() {
    clearBytes(serialInput.line, sizeof(serialInput.line));
    serialInput.used = 0;
    serialInput.overflow = false;
    serialInput.hasNul = false;
}

bool saveSerialLine() {
    ConfigError error = ConfigError::None;
    ConfigBuffer config;
    bool parsed = false;
    if (!serialInput.overflow && !serialInput.hasNul) {
        serialInput.line[serialInput.used] = '\0';
        parsed = parseConfig(serialInput.line, config.value, error);
    }
    const bool overflow = serialInput.overflow;
    resetSerialLine();
    if (overflow) {
        Serial.println("Config rejected: too long");
        return false;
    }
    if (!parsed && error == ConfigError::None) {
        Serial.println("Config rejected: invalid JSON");
        return false;
    }
    if (!parsed || !saveConfig(config.value, error)) {
        Serial.println(configErrorText(error));
        return false;
    }
    return true;
}

void discardByte(int byte) {
    serialInput.discardLine = byte != '\n';
    if (serialInput.pendingDiscard) --serialInput.pendingDiscard;
}
}

const char* configErrorText(ConfigError error) {
    switch (error) {
    case ConfigError::None: return "";
    case ConfigError::Ssid: return "Enter a Wi-Fi name (1-32 bytes)";
    case ConfigError::Password: return "Wi-Fi password exceeds 64 bytes";
    case ConfigError::Host: return "Use a camera hostname or IPv4 address";
    case ConfigError::Token: return "Invalid pairing token";
    case ConfigError::TokenTooShort: return "Pairing token needs 24 characters";
    case ConfigError::ControlPort: return "Control port must be 1-65535";
    case ConfigError::StreamPort: return "Stream port must be 1-65535";
    case ConfigError::StorageUnavailable: return "Config storage unavailable";
    case ConfigError::BackupWrite: return "Config backup write failed";
    case ConfigError::BackupReadback: return "Config backup verification failed";
    case ConfigError::PrimaryWrite: return "Config write failed";
    case ConfigError::PrimaryReadback: return "Config verification failed";
    case ConfigError::Inactive: return "Open Setup to edit configuration";
    }
    return "Invalid configuration";
}

bool loadConfig(ViewerConfig& config) {
    Preferences prefs;
    if (!prefs.begin(storageNamespace, true)) return false;

    ConfigBuffer loaded;
    if (readBlob(prefs, primaryKey, loaded.value)) {
        config = loaded.value;
        return true;
    }
    if (!readBlob(prefs, previousKey, loaded.value)) return false;

    protectPrevious = true;
    config = loaded.value;
    Serial.println("Config recovered from previous record");
    return true;
}

bool saveConfig(const ViewerConfig& config, ConfigError& error) {
    if (!validateConfig(config, error)) return false;
    Preferences prefs;
    if (!prefs.begin(storageNamespace, false)) return reject(error, ConfigError::StorageUnavailable);

    if (!protectPrevious) {
        ConfigBuffer stored;
        if (readBlob(prefs, primaryKey, stored.value)) {
            if (!checkedWrite(prefs, previousKey, stored.value,
                              ConfigError::BackupWrite, ConfigError::BackupReadback, error)) return false;
            protectPrevious = true;
        } else {
            protectPrevious = readBlob(prefs, previousKey, stored.value);
        }
    }
    if (!checkedWrite(prefs, primaryKey, config,
                      ConfigError::PrimaryWrite, ConfigError::PrimaryReadback, error)) return false;
    protectPrevious = false;
    return true;
}

void restartAfterConfigSave() {
    Serial.println("Config saved; restarting");
    delay(100);
    ESP.restart();
}

void discardConfigSerialInput() {
    if (serialInput.used || serialInput.overflow || serialInput.hasNul) serialInput.discardLine = true;
    resetSerialLine();
    serialInput.pendingDiscard = Serial.available();
    for (size_t count = 0; count < serialBudget && serialInput.pendingDiscard; ++count) {
        const int byte = Serial.read();
        if (byte < 0) break;
        discardByte(byte);
    }
}

bool pollConfigSerial() {
    for (size_t count = 0; count < serialBudget && Serial.available(); ++count) {
        const int byte = Serial.read();
        if (byte < 0) break;
        if (serialInput.pendingDiscard || serialInput.discardLine) {
            discardByte(byte);
        } else if (byte == '\n') {
            if (!saveSerialLine()) continue;
            restartAfterConfigSave();
            return true;
        } else if (byte == '\0') {
            serialInput.hasNul = true;
        } else if (byte != '\r') {
            if (serialInput.used < sizeof(serialInput.line) - 1) {
                serialInput.line[serialInput.used++] = static_cast<char>(byte);
            } else {
                serialInput.overflow = true;
            }
        }
    }
    return false;
}
}
