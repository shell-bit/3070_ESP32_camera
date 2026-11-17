#include "viewer_config.h"

#include <Arduino.h>
#include <MD5Builder.h>
#include <Preferences.h>
#include <cJSON.h>
#include <nvs.h>
#include <cstddef>
#include <cstring>

namespace lcd {
namespace {
constexpr char storageNamespace[] = "lcd-preview";
constexpr char primaryKey[] = "config";
constexpr char previousKey[] = "config-prev";
constexpr char setupBuildKey[] = "setup-build";
constexpr size_t serialBudget = 512;

static_assert(sizeof(ViewerConfig) == 360);
static_assert(offsetof(ViewerConfig, controlPort) == 356);
static_assert(offsetof(ViewerConfig, streamPort) == 358);

// Only the setup/serial owner accesses NVS after boot. Hold the recovery
// record across retries when a primary write may have committed but failed verification.
bool protectPrevious = false;
bool loadedFromPrevious = false;

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

bool storedBlobPresent(nvs_handle_t handle, const char* key, bool& present) {
    size_t length = 0;
    const esp_err_t result = nvs_get_blob(handle, key, nullptr, &length);
    present = result == ESP_OK;
    return result == ESP_OK || result == ESP_ERR_NVS_NOT_FOUND;
}

bool inspectSetupKeys(bool& primary, bool& previous, bool& revision) {
    primary = previous = revision = false;
    nvs_handle_t handle;
    const esp_err_t result = nvs_open(storageNamespace, NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return true;
    if (result != ESP_OK) return false;
    // Preferences::isKey/getBytesLength collapse metadata errors into absence.
    // Only an explicit NOT_FOUND may authorize first-time initialization.
    const bool readable = storedBlobPresent(handle, primaryKey, primary) &&
                          storedBlobPresent(handle, previousKey, previous) &&
                          storedBlobPresent(handle, setupBuildKey, revision);
    nvs_close(handle);
    return readable;
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

void deleteConfigJson(cJSON* json) {
    if (cJSON_IsObject(json)) {
        for (auto* item = json->child; item; item = item->next) {
            if (cJSON_IsString(item) && item->valuestring) {
                clearBytes(item->valuestring, std::strlen(item->valuestring));
            }
        }
    }
    cJSON_Delete(json);
}

bool uniqueSetupKeys(const cJSON* json) {
    constexpr const char* keys[] = {
        "wifi_ssid", "wifi_password", "camera_host", "token", "control_port", "stream_port"
    };
    unsigned seen = 0;
    for (const auto* item = json->child; item; item = item->next) {
        unsigned bit = 0;
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
            if (item->string && std::strcmp(item->string, keys[i]) == 0) bit = 1U << i;
        }
        if (!bit || (seen & bit)) return false;
        seen |= bit;
    }
    return true;
}

bool parseConfig(const char* line, ViewerConfig& config, ConfigError& error, bool strict = false) {
    if (hasInvalidUnicodeEscape(line)) return false;
    auto* json = cJSON_ParseWithOpts(line, nullptr, true);
    bool parsed = false;
    if (cJSON_IsObject(json) && (!strict || uniqueSetupKeys(json))) {
        if (!readString(json, "wifi_ssid", config.ssid)) error = ConfigError::Ssid;
        else if (!readString(json, "wifi_password", config.password)) error = ConfigError::Password;
        else if (!readString(json, "camera_host", config.host)) error = ConfigError::Host;
        else if (!readString(json, "token", config.token)) error = ConfigError::Token;
        else if (!readPort(json, "control_port", config.controlPort)) error = ConfigError::ControlPort;
        else if (!readPort(json, "stream_port", config.streamPort)) error = ConfigError::StreamPort;
        else parsed = true;
    }
    deleteConfigJson(json);
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
    loadedFromPrevious = false;
    Preferences prefs;
    if (!prefs.begin(storageNamespace, true)) return false;

    ConfigBuffer loaded;
    if (readBlob(prefs, primaryKey, loaded.value)) {
        config = loaded.value;
        return true;
    }
    if (!readBlob(prefs, previousKey, loaded.value)) return false;

    protectPrevious = true;
    loadedFromPrevious = true;
    config = loaded.value;
    Serial.println("Config recovered from previous record");
    return true;
}

namespace {
bool writeConfig(Preferences& prefs, const ViewerConfig& config, ConfigError& error,
                 const ViewerConfig* verifiedPrimary = nullptr) {
    if (!protectPrevious) {
        ConfigBuffer stored;
        if (verifiedPrimary) std::memcpy(&stored.value, verifiedPrimary, sizeof(stored.value));
        if (verifiedPrimary || readBlob(prefs, primaryKey, stored.value)) {
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
}

bool saveConfig(const ViewerConfig& config, ConfigError& error) {
    if (!validateConfig(config, error)) return false;
    Preferences prefs;
    if (!prefs.begin(storageNamespace, false)) return reject(error, ConfigError::StorageUnavailable);
    return writeConfig(prefs, config, error);
}

void applyBuiltSetupConfig(ViewerConfig& config, const char* json, std::size_t length) {
    if (!json || !length || length > 1024 || std::memchr(json, '\0', length)) {
        Serial.println("Built setup config rejected: invalid setup JSON");
        return;
    }

    // Parse only the supplied bytes, including callers without a trailing NUL.
    char input[1025];
    std::memcpy(input, json, length);
    input[length] = '\0';
    ConfigBuffer candidate;
    // Include padding in the deterministic representation used for NVS readback
    // and retry comparisons; a marker retry must not rotate the backup again.
    clearBytes(&candidate.value, sizeof(candidate.value));
    candidate.value.controlPort = ViewerConfig{}.controlPort;
    candidate.value.streamPort = ViewerConfig{}.streamPort;
    ConfigError error = ConfigError::None;
    const bool parsed = parseConfig(input, candidate.value, error, true);
    clearBytes(input, length);
    if (!parsed || !validateConfig(candidate.value, error) ||
        (candidate.value.token[0] && std::strlen(candidate.value.token) < 24)) {
        Serial.println("Built setup config rejected: invalid setup JSON");
        return;
    }

    // Identify the JSON already applied so an ordinary reboot can retain touchscreen edits.
    MD5Builder digest;
    digest.begin();
    digest.add(reinterpret_cast<const uint8_t*>(json), length);
    digest.calculate();
    char revision[33];
    digest.getChars(revision);

    bool hasPrimary, hasPrevious, hasRevision;
    if (!inspectSetupKeys(hasPrimary, hasPrevious, hasRevision)) {
        Serial.println("Setup config deferred: could not inspect saved settings");
        return;
    }
    Preferences prefs;
    if (!prefs.begin(storageNamespace, false)) {
        Serial.println(configErrorText(ConfigError::StorageUnavailable));
        return;
    }
    ConfigBuffer stored;
    const bool primaryReadable = readBlob(prefs, primaryKey, stored.value);
    // An unreadable existing record is not a factory-new device. Keep recovery
    // data intact even if a subsequent read after load happens to succeed.
    if (loadedFromPrevious || (hasPrimary && !primaryReadable) ||
        (!primaryReadable && hasPrevious)) {
        Serial.println("Setup config deferred: could not read primary settings");
        return;
    }
    if (primaryReadable) config = stored.value;

    char applied[sizeof(revision)];
    if (hasRevision) {
        if (prefs.getBytesLength(setupBuildKey) != sizeof(applied) ||
            prefs.getBytes(setupBuildKey, applied, sizeof(applied)) != sizeof(applied)) {
            Serial.println("Setup config deferred: could not read firmware revision");
            return;
        }
        if (primaryReadable && std::memcmp(applied, revision, sizeof(applied)) == 0) return;
    }

    if (!primaryReadable || std::memcmp(&stored.value, &candidate.value, sizeof(config)) != 0) {
        if (!writeConfig(prefs, candidate.value, error, primaryReadable ? &stored.value : nullptr)) {
            Serial.println(configErrorText(error));
            return;
        }
        config = candidate.value;
    }
    // On interruption or marker failure, retry on boot without rewriting matching settings.
    if (prefs.putBytes(setupBuildKey, revision, sizeof(revision)) != sizeof(revision)) {
        Serial.println("Setup config saved; could not record firmware revision");
        return;
    }
    Serial.println("Setup config applied from firmware");
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
