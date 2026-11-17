#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace fake_nvs {
using Blob = std::vector<uint8_t>;
inline std::map<std::string, Blob> records;
inline std::map<std::string, unsigned> writes;
inline bool namespaceExists = false;
inline int32_t nativeOpenError = 0;
inline std::map<std::string, int32_t> metadataErrors;

inline void failMetadataOnce(const std::string& key, int32_t error = -1) {
    metadataErrors[key] = error;
}
inline void failNativeOpenOnce(int32_t error = -1) { nativeOpenError = error; }

enum class Operation { OpenRead, OpenWrite, Length, Read, Write, CommittedWrite, CorruptRead };
struct Fault {
    Operation operation;
    std::string key;
    unsigned skip;
    bool waitingForWrite;
};
inline std::vector<Fault> faults;

inline void failOnce(Operation operation, const std::string& key = {}, unsigned skip = 0) {
    faults.push_back({operation, key, skip, false});
}
inline void failReadbackOnce(const std::string& key, bool corrupt = false) {
    faults.push_back({corrupt ? Operation::CorruptRead : Operation::Read, key, 0, true});
}
inline bool consume(Operation operation, const std::string& key) {
    for (auto it = faults.begin(); it != faults.end(); ++it) {
        if (it->operation != operation || it->waitingForWrite ||
            (!it->key.empty() && it->key != key)) continue;
        if (it->skip) {
            --it->skip;
            return false;
        }
        faults.erase(it);
        return true;
    }
    return false;
}
inline void wrote(const std::string& key) {
    for (auto& fault : faults) {
        if (fault.key == key) fault.waitingForWrite = false;
    }
}
inline unsigned writeCount() {
    unsigned result = 0;
    for (const auto& entry : writes) result += entry.second;
    return result;
}
inline void reset() {
    records.clear();
    writes.clear();
    faults.clear();
    namespaceExists = false;
    nativeOpenError = 0;
    metadataErrors.clear();
}
}

class Preferences {
public:
    bool begin(const char* name, bool readOnly = false) {
        readOnly_ = readOnly;
        opened_ = std::strcmp(name, "lcd-preview") == 0 &&
                  !fake_nvs::consume(readOnly ? fake_nvs::Operation::OpenRead :
                                               fake_nvs::Operation::OpenWrite, {});
        if (opened_ && !readOnly) fake_nvs::namespaceExists = true;
        return opened_;
    }
    void end() { opened_ = false; }
    bool isKey(const char* key) const {
        return opened_ && fake_nvs::records.count(key) != 0;
    }
    std::size_t getBytesLength(const char* key) const {
        if (!isKey(key) || fake_nvs::consume(fake_nvs::Operation::Length, key)) return 0;
        return fake_nvs::records.at(key).size();
    }
    std::size_t getBytes(const char* key, void* destination, std::size_t maximum) const {
        if (!isKey(key) || fake_nvs::consume(fake_nvs::Operation::Read, key)) return 0;
        const auto& stored = fake_nvs::records.at(key);
        if (stored.size() > maximum) return 0;
        std::memcpy(destination, stored.data(), stored.size());
        if (!stored.empty() && fake_nvs::consume(fake_nvs::Operation::CorruptRead, key)) {
            static_cast<uint8_t*>(destination)[0] ^= 1;
        }
        return stored.size();
    }
    std::size_t putBytes(const char* key, const void* source, std::size_t length) {
        if (!opened_ || readOnly_) return 0;
        ++fake_nvs::writes[key];
        if (fake_nvs::consume(fake_nvs::Operation::Write, key)) return 0;
        const auto* bytes = static_cast<const uint8_t*>(source);
        fake_nvs::records[key] = fake_nvs::Blob(bytes, bytes + length);
        fake_nvs::wrote(key);
        // An error return can be ambiguous: the bytes may already be committed.
        if (fake_nvs::consume(fake_nvs::Operation::CommittedWrite, key)) return 0;
        return length;
    }

private:
    bool opened_ = false;
    bool readOnly_ = true;
};
