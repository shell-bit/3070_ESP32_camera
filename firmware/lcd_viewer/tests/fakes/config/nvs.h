#pragma once

#include <Preferences.h>

using esp_err_t = int32_t;
using nvs_handle_t = uint32_t;
enum nvs_open_mode_t { NVS_READONLY, NVS_READWRITE };
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_NVS_NOT_FOUND = 0x1102;
constexpr esp_err_t ESP_ERR_NVS_TYPE_MISMATCH = 0x1103;

inline esp_err_t nvs_open(const char* name, nvs_open_mode_t mode, nvs_handle_t* handle) {
    if (fake_nvs::nativeOpenError != ESP_OK) {
        const auto error = fake_nvs::nativeOpenError;
        fake_nvs::nativeOpenError = ESP_OK;
        return error;
    }
    if (std::strcmp(name, "lcd-preview") != 0) return ESP_ERR_NVS_NOT_FOUND;
    if (mode == NVS_READWRITE) fake_nvs::namespaceExists = true;
    if (!fake_nvs::namespaceExists && fake_nvs::records.empty()) return ESP_ERR_NVS_NOT_FOUND;
    *handle = 1;
    return ESP_OK;
}

inline esp_err_t nvs_get_blob(nvs_handle_t handle, const char* key, void* output,
                             std::size_t* length) {
    if (handle != 1 || !length) return ESP_FAIL;
    const auto failure = fake_nvs::metadataErrors.find(key);
    if (failure != fake_nvs::metadataErrors.end()) {
        const auto error = failure->second;
        fake_nvs::metadataErrors.erase(failure);
        return error;
    }
    const auto found = fake_nvs::records.find(key);
    if (found == fake_nvs::records.end()) return ESP_ERR_NVS_NOT_FOUND;
    if (output) {
        if (*length < found->second.size()) return ESP_FAIL;
        std::memcpy(output, found->second.data(), found->second.size());
    }
    *length = found->second.size();
    return ESP_OK;
}

inline void nvs_close(nvs_handle_t) {}
