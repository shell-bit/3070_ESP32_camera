#include "../src/config/viewer_config.h"

#include <Arduino.h>
#include <Preferences.h>
#include <nvs.h>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using lcd::ViewerConfig;
using fake_nvs::Operation;

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error( \
    "line " + std::to_string(__LINE__) + ": " #condition); } while (false)

constexpr char pairingToken[] = "fixture-pairing-token-0123456789";

ViewerConfig fixture(const char* ssid = "fixture-old", const char* password = "old-password",
                     const char* host = "old-camera.test", const char* token = pairingToken,
                     uint16_t control = 8080, uint16_t stream = 8081) {
    ViewerConfig value{};
    std::strcpy(value.ssid, ssid);
    std::strcpy(value.password, password);
    std::strcpy(value.host, host);
    std::strcpy(value.token, token);
    value.controlPort = control;
    value.streamPort = stream;
    return value;
}

std::string payload(const std::string& ssid = "fixture-new",
                    const std::string& password = "new-password",
                    const std::string& host = "new-camera.test",
                    const std::string& token = pairingToken,
                    const std::string& extra = {}) {
    // Callers supply only synthetic strings or intentional raw JSON escapes.
    return "{\"wifi_ssid\":\"" + ssid + "\",\"wifi_password\":\"" + password +
           "\",\"camera_host\":\"" + host + "\",\"token\":\"" + token + "\"" + extra + "}";
}

std::string replace(std::string source, const std::string& from, const std::string& to) {
    const auto position = source.find(from);
    CHECK(position != std::string::npos);
    source.replace(position, from.size(), to);
    return source;
}

void expectConfig(const ViewerConfig& actual, const ViewerConfig& expected) {
    CHECK(std::strcmp(actual.ssid, expected.ssid) == 0);
    CHECK(std::strcmp(actual.password, expected.password) == 0);
    CHECK(std::strcmp(actual.host, expected.host) == 0);
    CHECK(std::strcmp(actual.token, expected.token) == 0);
    CHECK(actual.controlPort == expected.controlPort);
    CHECK(actual.streamPort == expected.streamPort);
}

void seed(const char* key, const ViewerConfig& value) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
    fake_nvs::records[key] = fake_nvs::Blob(bytes, bytes + sizeof(value));
}

ViewerConfig stored(const char* key) {
    CHECK(fake_nvs::records.count(key) == 1);
    const auto& bytes = fake_nvs::records.at(key);
    CHECK(bytes.size() == sizeof(ViewerConfig));
    ViewerConfig value{};
    std::memcpy(&value, bytes.data(), bytes.size());
    return value;
}

ViewerConfig boot(bool expectedLoaded = true) {
    ViewerConfig value{};
    CHECK(lcd::loadConfig(value) == expectedLoaded);
    return value;
}

void apply(ViewerConfig& current, const std::string& json = payload()) {
    lcd::applyBuiltSetupConfig(current, json.data(), json.size());
}

ViewerConfig expectedNew() {
    return fixture("fixture-new", "new-password", "new-camera.test", pairingToken, 80, 81);
}

void expectNew(const ViewerConfig& current) {
    expectConfig(current, expectedNew());
    expectConfig(stored("config"), expectedNew());
    CHECK(fake_nvs::records.count("setup-build") == 1);
}

void freshConfiguration() {
    auto current = boot(false);
    apply(current);
    expectNew(current);
    expectConfig(boot(), expectedNew());
    CHECK(fake_nvs::records.count("config-prev") == 0);
    CHECK(Serial.output.find("Setup config applied from firmware") != std::string::npos);
    CHECK(Serial.output.find("new-password") == std::string::npos);
    CHECK(Serial.output.find(pairingToken) == std::string::npos);
}

void fullReplacement() {
    seed("config", fixture());
    const auto original = fake_nvs::records.at("config");
    auto current = boot();
    apply(current);
    expectNew(current);
    CHECK(fake_nvs::records.at("config-prev") == original);
}

void unchangedPayloadPreservesSavedEdits() {
    auto current = boot(false);
    const auto json = payload();
    apply(current, json);
    const auto edited = fixture("touch-edited", "touch-password", "touch-camera.test",
                                "legacy-short-token", 9000, 9001);
    lcd::ConfigError error;
    CHECK(lcd::saveConfig(edited, error));
    const auto records = fake_nvs::records;
    const auto writes = fake_nvs::writeCount();
    current = boot();
    apply(current, json);
    expectConfig(current, edited);
    CHECK(fake_nvs::records == records);
    CHECK(fake_nvs::writeCount() == writes);
}

void changedWifiAndPorts() {
    auto current = boot(false);
    apply(current);
    apply(current, payload("wifi-revision", "wifi-revised-password"));
    expectConfig(current, fixture("wifi-revision", "wifi-revised-password", "new-camera.test",
                                  pairingToken, 80, 81));
    const auto wifiRevision = fake_nvs::records.at("config");
    apply(current, payload("wifi-revision", "wifi-revised-password", "new-camera.test",
                           pairingToken, ",\"control_port\":65535,\"stream_port\":1"));
    expectConfig(current, fixture("wifi-revision", "wifi-revised-password", "new-camera.test",
                                  pairingToken, 65535, 1));
    CHECK(fake_nvs::records.at("config-prev") == wifiRevision);
}

void rawWhitespaceRevisionReapplies() {
    auto current = boot(false);
    const auto json = payload();
    apply(current, json);
    lcd::ConfigError error;
    CHECK(lcd::saveConfig(fixture(), error));
    current = boot();
    apply(current, " \n" + json + "\n");
    expectNew(current);
}

void legacyMarkerIsInert() {
    auto current = boot(false);
    apply(current);
    const auto matchingMarker = fake_nvs::records.at("setup-build");
    fake_nvs::reset();
    seed("config", fixture());
    fake_nvs::records["camera-build"] = matchingMarker;
    current = boot();
    apply(current);
    expectNew(current);
    CHECK(fake_nvs::records.at("camera-build") == matchingMarker);
    CHECK(fake_nvs::writes.count("camera-build") == 0);
}

void optionalGroupsAndOpenNetwork() {
    seed("config", fixture());
    auto current = boot();
    apply(current, payload("open-fixture", "", "", ""));
    expectConfig(current, fixture("open-fixture", "", "", "", 80, 81));
    CHECK(current.wifiReady());
    CHECK(!current.cameraReady());
    apply(current, payload("", ""));
    expectConfig(current, fixture("", "", "new-camera.test", pairingToken, 80, 81));
    CHECK(!current.wifiReady());
    CHECK(current.cameraReady());
    expectConfig(boot(), current);
}

void maximumStringLengthsAndUtf8() {
    auto current = boot(false);
    apply(current, payload(std::string(32, 's'), std::string(64, 'p'),
                           std::string(127, 'h'), std::string(128, 't')));
    CHECK(std::strlen(current.ssid) == 32);
    CHECK(std::strlen(current.password) == 64);
    CHECK(std::strlen(current.host) == 127);
    CHECK(std::strlen(current.token) == 128);
    apply(current, payload("\xE6\xB5\x8B\xE8\xAF\x95", "\\u5bc6\\u7801"));
    CHECK(std::strcmp(current.ssid, "\xE6\xB5\x8B\xE8\xAF\x95") == 0);
    CHECK(std::strcmp(current.password, "\xE5\xAF\x86\xE7\xA0\x81") == 0);
}

void rejectedInputNeverWrites() {
    seed("config", fixture());
    auto current = boot();
    const auto original = fake_nvs::records;
    const auto json = payload();
    std::vector<std::string> invalid = {
        "", "{", "[]", "null", "{\"camera_host\":\"camera.test\",\"token\":\"short\"}",
        json + " trailing", json + " {}", "/* comment */" + json,
        replace(json, "\"wifi_ssid\":\"fixture-new\",", ""),
        replace(json, "\"wifi_password\":\"new-password\",", ""),
        replace(json, "\"camera_host\":\"new-camera.test\",", ""),
        replace(json, ",\"token\":\"" + std::string(pairingToken) + "\"", ""),
        replace(json, "\"fixture-new\"", "false"),
        payload("fixture", "password", "camera.test", pairingToken, ",\"unknown\":0"),
        payload("fixture", "password", "camera.test", pairingToken, ",\"wifi_ssid\":\"again\""),
        payload("fixture", "password", "camera.test", pairingToken,
                ",\"control_port\":80,\"control_port\":81"),
        payload("fixture", "password", "camera.test", pairingToken, ",\"wifi_\\u0073sid\":\"again\""),
        payload(std::string(33, 's')), payload("fixture", std::string(65, 'p')),
        payload("fixture", "password", std::string(128, 'h')),
        payload("fixture", "password", "camera.test", std::string(129, 't')),
        payload("fixture", "password", "camera.test", "short"),
        payload("fixture", "password", "http://camera.test"),
        payload("fixture", "password", "camera.test", "invalid token with spaces"),
        payload("", "orphan-password", "", ""), payload("fixture", "password", "", pairingToken),
        payload("fixture", "password", "camera.test", ""), payload("", "", "", ""),
        payload("bad\\u0000tail"), payload("bad\\uQQQQ"), payload("bad\\u12"),
        payload("bad\\uD800"), payload("bad\\uDC00"),
        replace(json, "wifi_ssid", "wifi_ssid\\u0000hidden"),
        json + std::string(1025, ' ')
    };
    for (const auto* port : {"control_port", "stream_port"}) {
        for (const auto* value : {"0", "65536", "-1", "1.5", "true", "\"80\"", "null", "1e999"}) {
            invalid.push_back(payload("fixture", "password", "camera.test", pairingToken,
                                      ",\"" + std::string(port) + "\":" + value));
        }
    }
    auto embeddedNul = json;
    embeddedNul.insert(10, 1, '\0');
    invalid.push_back(embeddedNul);
    for (std::size_t index = 0; index < invalid.size(); ++index) {
        apply(current, invalid[index]);
        CHECK(fake_nvs::records == original);
        CHECK(fake_nvs::writeCount() == 0);
        expectConfig(current, fixture());
    }
    lcd::applyBuiltSetupConfig(current, nullptr, 1);
    CHECK(fake_nvs::records == original);
    CHECK(fake_nvs::writeCount() == 0);
}

void explicitLengthBoundsParsing() {
    auto current = boot(false);
    const auto json = payload();
    std::vector<char> unterminated(json.begin(), json.end());
    lcd::applyBuiltSetupConfig(current, unterminated.data(), unterminated.size());
    expectNew(current);
    // Bytes outside the caller's span must not change parsing or revision identity.
    const auto records = fake_nvs::records;
    const auto writes = fake_nvs::writeCount();
    const auto withSuffix = json + " not JSON";
    lcd::applyBuiltSetupConfig(current, withSuffix.data(), json.size());
    CHECK(fake_nvs::records == records);
    CHECK(fake_nvs::writeCount() == writes);
    lcd::applyBuiltSetupConfig(current, json.data(), json.size() - 1);
    CHECK(fake_nvs::records == records);
    CHECK(fake_nvs::writeCount() == writes);
}

void exactInputLimit() {
    auto current = boot(false);
    auto json = payload();
    json.append(1024 - json.size(), ' ');
    apply(current, json);
    expectNew(current);
    const auto records = fake_nvs::records;
    const auto writes = fake_nvs::writeCount();
    apply(current, json + ' ');
    CHECK(fake_nvs::records == records);
    CHECK(fake_nvs::writeCount() == writes);
}

void missingRecordsWithSameMarkerReconstruct() {
    auto current = boot(false);
    apply(current);
    const auto marker = fake_nvs::records.at("setup-build");
    fake_nvs::records.erase("config");
    fake_nvs::records.erase("config-prev");
    current = boot(false);
    apply(current);
    expectNew(current);
    CHECK(fake_nvs::records.at("setup-build") == marker);
    CHECK(fake_nvs::records.count("config-prev") == 0);
}

void existingCorruptRecordDefers() {
    fake_nvs::records["config"] = {1, 2, 3};
    auto current = boot(false);
    const auto records = fake_nvs::records;
    apply(current);
    CHECK(fake_nvs::records == records);
    CHECK(fake_nvs::writeCount() == 0);
    CHECK(!current.ready());
}

void previousOnlyDefers() {
    seed("config-prev", fixture());
    auto current = boot();
    const auto records = fake_nvs::records;
    apply(current);
    expectConfig(current, fixture());
    CHECK(fake_nvs::records == records);
    CHECK(fake_nvs::writeCount() == 0);
}

void corruptPreviousOnlyDefers() {
    fake_nvs::records["config-prev"] = {9};
    auto current = boot(false);
    const auto records = fake_nvs::records;
    apply(current);
    CHECK(fake_nvs::records == records);
    CHECK(fake_nvs::writeCount() == 0);
}

void recoveredBootDefersUntilNextLoad() {
    seed("config", fixture("primary-wifi"));
    seed("config-prev", fixture("previous-wifi"));
    fake_nvs::failOnce(Operation::Read, "config");
    auto current = boot();
    expectConfig(current, fixture("previous-wifi"));
    const auto records = fake_nvs::records;
    // The primary is readable now, but this boot already selected recovery.
    apply(current);
    expectConfig(current, fixture("previous-wifi"));
    CHECK(fake_nvs::records == records);
    CHECK(fake_nvs::writeCount() == 0);
    current = boot();
    apply(current);
    expectNew(current);
    // loadConfig resets the embedded-update guard, not the save retry guard.
    // In this same process, keep the recovery record that is still protected.
    expectConfig(stored("config-prev"), fixture("previous-wifi"));
}

void metadataFailuresNeverLookLikeEmptyStorage() {
    seed("config", fixture());
    seed("config-prev", fixture("previous-wifi"));
    auto current = boot();
    const auto records = fake_nvs::records;
    for (const auto* key : {"config", "config-prev", "setup-build"}) {
        for (const auto error : {ESP_FAIL, ESP_ERR_NVS_TYPE_MISMATCH}) {
            fake_nvs::failMetadataOnce(key, error);
            apply(current);
            expectConfig(current, fixture());
            CHECK(fake_nvs::records == records);
            CHECK(fake_nvs::writeCount() == 0);
        }
    }
    fake_nvs::failNativeOpenOnce();
    apply(current);
    expectConfig(current, fixture());
    CHECK(fake_nvs::records == records);
    CHECK(fake_nvs::writeCount() == 0);
    apply(current);
    expectNew(current);
}

void sameMarkerWithUnreadablePrimaryDefers() {
    auto current = boot(false);
    apply(current);
    seed("config-prev", fixture());
    fake_nvs::records["config"] = {0};
    current = boot();
    const auto records = fake_nvs::records;
    const auto writes = fake_nvs::writeCount();
    apply(current);
    expectConfig(current, fixture());
    CHECK(fake_nvs::records == records);
    CHECK(fake_nvs::writeCount() == writes);
}

void readablePrimaryFaultDefers() {
    seed("config", fixture());
    auto current = boot();
    const auto records = fake_nvs::records;
    fake_nvs::failOnce(Operation::Read, "config");
    apply(current);
    expectConfig(current, fixture());
    CHECK(fake_nvs::records == records);
    CHECK(fake_nvs::writeCount() == 0);
    apply(current);
    expectNew(current);
}

void markerReadFailureDefers() {
    auto current = boot(false);
    apply(current);
    const auto records = fake_nvs::records;
    const auto writes = fake_nvs::writeCount();
    for (const auto operation : {Operation::Length, Operation::Read}) {
        fake_nvs::failOnce(operation, "setup-build");
        apply(current, payload("revised-wifi"));
        expectConfig(current, expectedNew());
        CHECK(fake_nvs::records == records);
        CHECK(fake_nvs::writeCount() == writes);
    }
    fake_nvs::records["setup-build"] = {0};
    const auto corruptMarkerRecords = fake_nvs::records;
    apply(current, payload("revised-wifi"));
    CHECK(fake_nvs::records == corruptMarkerRecords);
    CHECK(fake_nvs::writeCount() == writes);
}

void storageOpenFailureRetries() {
    seed("config", fixture());
    auto current = boot();
    const auto records = fake_nvs::records;
    fake_nvs::failOnce(Operation::OpenWrite);
    apply(current);
    expectConfig(current, fixture());
    CHECK(fake_nvs::records == records);
    CHECK(fake_nvs::writeCount() == 0);
    apply(current);
    expectNew(current);
}

void backupWriteFailureRetries() {
    seed("config", fixture());
    seed("config-prev", fixture("older-wifi"));
    auto current = boot();
    const auto records = fake_nvs::records;
    fake_nvs::failOnce(Operation::Write, "config-prev");
    apply(current);
    expectConfig(current, fixture());
    CHECK(fake_nvs::records == records);
    CHECK(fake_nvs::writes.count("config") == 0);
    CHECK(fake_nvs::writes.count("setup-build") == 0);
    apply(current);
    expectNew(current);
    expectConfig(stored("config-prev"), fixture());
}

void backupReadbackFailureRetries() {
    seed("config", fixture());
    auto current = boot();
    const auto original = fake_nvs::records.at("config");
    fake_nvs::failReadbackOnce("config-prev");
    apply(current);
    expectConfig(current, fixture());
    CHECK(fake_nvs::records.at("config") == original);
    CHECK(fake_nvs::records.at("config-prev") == original);
    CHECK(fake_nvs::writes.count("config") == 0);
    CHECK(fake_nvs::writes.count("setup-build") == 0);
    apply(current);
    expectNew(current);
    CHECK(fake_nvs::records.at("config-prev") == original);
}

void primaryFailureRetries(Operation operation, bool readback, bool corrupt = false) {
    seed("config", fixture());
    auto current = boot();
    const auto original = fake_nvs::records.at("config");
    if (readback) fake_nvs::failReadbackOnce("config", corrupt);
    else fake_nvs::failOnce(operation, "config");
    apply(current);
    expectConfig(current, fixture());
    CHECK(fake_nvs::records.at("config-prev") == original);
    CHECK(fake_nvs::records.count("setup-build") == 0);
    if (operation == Operation::Write && !readback) {
        CHECK(fake_nvs::records.at("config") == original);
    }
    const auto backupWrites = fake_nvs::writes.at("config-prev");
    apply(current);
    expectNew(current);
    CHECK(fake_nvs::records.at("config-prev") == original);
    CHECK(fake_nvs::writes.at("config-prev") == backupWrites);
}

void primaryWriteFailureRetries() { primaryFailureRetries(Operation::Write, false); }
void primaryCommittedErrorRetries() { primaryFailureRetries(Operation::CommittedWrite, false); }
void primaryReadbackFailureRetries() { primaryFailureRetries(Operation::Read, true); }
void primaryReadbackMismatchRetries() { primaryFailureRetries(Operation::Read, true, true); }

void markerFailureRetriesWithoutBackupRotation() {
    seed("config", fixture());
    auto current = boot();
    const auto original = fake_nvs::records.at("config");
    fake_nvs::failOnce(Operation::Write, "setup-build");
    apply(current);
    expectConfig(current, expectedNew());
    CHECK(fake_nvs::records.count("setup-build") == 0);
    CHECK(fake_nvs::records.at("config-prev") == original);
    const auto primaryWrites = fake_nvs::writes.at("config");
    const auto backupWrites = fake_nvs::writes.at("config-prev");
    current = boot();
    apply(current);
    expectNew(current);
    CHECK(fake_nvs::records.at("config-prev") == original);
    CHECK(fake_nvs::writes.at("config") == primaryWrites);
    CHECK(fake_nvs::writes.at("config-prev") == backupWrites);
}

void firstBootWriteFailureRetries() {
    auto current = boot(false);
    fake_nvs::failOnce(Operation::Write, "config");
    apply(current);
    CHECK(!current.wifiReady());
    CHECK(fake_nvs::records.empty());
    apply(current);
    expectNew(current);
    CHECK(fake_nvs::records.count("config-prev") == 0);
}

void usbRetainsLegacyContract() {
    seed("config", fixture());
    const auto original = fake_nvs::records.at("config");
    Serial.feed(payload("usb-wifi", "usb-password", "usb-camera.test", "short",
                        ",\"ignored_legacy_field\":true") + "\n");
    bool saved = false;
    while (Serial.available()) saved = lcd::pollConfigSerial() || saved;
    CHECK(saved);
    CHECK(ESP.restarts == 1);
    expectConfig(boot(), fixture("usb-wifi", "usb-password", "usb-camera.test", "short", 80, 81));
    CHECK(fake_nvs::records.at("config-prev") == original);
    CHECK(fake_nvs::records.count("setup-build") == 0);
    CHECK(Serial.output.find("usb-password") == std::string::npos);
}

const std::pair<const char*, void (*)()> cases[] = {
    {"fresh_configuration", freshConfiguration},
    {"full_replacement", fullReplacement},
    {"unchanged_payload_preserves_saved_edits", unchangedPayloadPreservesSavedEdits},
    {"changed_wifi_and_ports", changedWifiAndPorts},
    {"raw_whitespace_revision_reapplies", rawWhitespaceRevisionReapplies},
    {"legacy_marker_is_inert", legacyMarkerIsInert},
    {"optional_groups_and_open_network", optionalGroupsAndOpenNetwork},
    {"maximum_string_lengths_and_utf8", maximumStringLengthsAndUtf8},
    {"rejected_input_never_writes", rejectedInputNeverWrites},
    {"explicit_length_bounds_parsing", explicitLengthBoundsParsing},
    {"exact_input_limit", exactInputLimit},
    {"missing_records_with_same_marker_reconstruct", missingRecordsWithSameMarkerReconstruct},
    {"existing_corrupt_record_defers", existingCorruptRecordDefers},
    {"previous_only_defers", previousOnlyDefers},
    {"corrupt_previous_only_defers", corruptPreviousOnlyDefers},
    {"recovered_boot_defers_until_next_load", recoveredBootDefersUntilNextLoad},
    {"metadata_failures_never_look_like_empty_storage", metadataFailuresNeverLookLikeEmptyStorage},
    {"same_marker_with_unreadable_primary_defers", sameMarkerWithUnreadablePrimaryDefers},
    {"readable_primary_fault_defers", readablePrimaryFaultDefers},
    {"marker_read_failure_defers", markerReadFailureDefers},
    {"storage_open_failure_retries", storageOpenFailureRetries},
    {"backup_write_failure_retries", backupWriteFailureRetries},
    {"backup_readback_failure_retries", backupReadbackFailureRetries},
    {"primary_write_failure_retries", primaryWriteFailureRetries},
    {"primary_committed_error_retries", primaryCommittedErrorRetries},
    {"primary_readback_failure_retries", primaryReadbackFailureRetries},
    {"primary_readback_mismatch_retries", primaryReadbackMismatchRetries},
    {"marker_failure_retries_without_backup_rotation", markerFailureRetriesWithoutBackupRotation},
    {"first_boot_write_failure_retries", firstBootWriteFailureRetries},
    {"usb_retains_legacy_contract", usbRetainsLegacyContract},
};
}

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--list") == 0) {
        for (const auto& entry : cases) std::cout << entry.first << '\n';
        return 0;
    }
    if (argc != 2) {
        std::cerr << "Pass one case name or --list; run each case in a fresh process.\n";
        return 2;
    }
    for (const auto& entry : cases) {
        if (std::strcmp(entry.first, argv[1]) != 0) continue;
        try {
            entry.second();
            std::cout << "PASS: " << entry.first << '\n';
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "FAIL: " << entry.first << ": " << error.what() << '\n';
            return 1;
        }
    }
    std::cerr << "Unknown configuration test case\n";
    return 2;
}
