#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

// Exact, process-local content identities for revision-flow tests only. This is
// deliberately not MD5 and provides no evidence about cryptographic behavior.
class MD5Builder {
public:
    void begin() { input_.clear(); }
    void add(const uint8_t* bytes, std::size_t length) {
        input_.append(reinterpret_cast<const char*>(bytes), length);
    }
    void calculate() {
        auto found = identities_.find(input_);
        if (found == identities_.end()) {
            found = identities_.emplace(input_, identities_.size() + 1).first;
        }
        std::snprintf(result_, sizeof(result_), "%032llx",
                      static_cast<unsigned long long>(found->second));
    }
    void getChars(char* destination) const { std::memcpy(destination, result_, sizeof(result_)); }

private:
    std::string input_;
    char result_[33]{};
    inline static std::map<std::string, std::size_t> identities_;
};
