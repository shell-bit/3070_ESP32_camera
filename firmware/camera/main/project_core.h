#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace camera {
bool validIdentifier(const std::string &s, size_t max = 128);
bool constantTimeEqual(const std::string &a, const std::string &b);
bool consistentSample(const std::vector<std::vector<float>> &accepted,
                      const std::vector<float> &candidate, float threshold = 0.75f);
enum class AlarmState { Unknown, Pending, Suspicious, Normal, Review, Exempted };
struct PendingTrigger {
  bool pending = false;
  uint64_t started = 0;
  void request(uint64_t now) {
    if (!pending) {
      pending = true;
      started = now;
    }
  }
  bool expired(uint64_t now) const {
    return pending && now - started >= 10000;
  }
  void consumed() {
    pending = false;
  }
};
AlarmState parseAlarm(const std::string &s);
struct Budget {
  uint64_t start, duration;
  Budget(uint64_t s, uint64_t d) : start(s), duration(d) {}
  bool available(uint64_t now) const {
    return now >= start && now - start < duration;
  }
  uint64_t remaining(uint64_t now) const {
    return available(now) ? duration - (now - start) : 0;
  }
};
// Callers serialize this state; deadlines use the monotonic ESP timer, not wall time.
struct PreviewSession {
  std::string request, id;
  uint64_t deadline = 0, generation = 0;
  bool valid(uint64_t now) const {
    return !id.empty() && now < deadline;
  }
  int open(const std::string &r, const std::string &newId, uint64_t now);
  bool renew(const std::string &sid, uint64_t now);
  bool close(const std::string &sid);
};
struct Template {
  std::string member, command;
  uint32_t version = 0;
  uint32_t databaseVersion = 0;
  bool revoked = false;
  std::vector<std::vector<float>> features;
};
struct Match {
  std::string member;
  uint32_t version = 0;
  float score = 0;
};
class Registry {
public:
  static constexpr size_t MaxMembers = 16, Samples = 3, MaxDimensions = 1024;
  std::vector<Template> entries;
  uint32_t version = 0;
  bool commit(const std::string &member, uint32_t memberVersion, const std::string &command,
              const std::vector<std::vector<float>> &features, float consistencyThreshold = 0.75f);
  bool revoke(const std::string &member, uint32_t memberVersion, const std::string &command);
  Match match(const std::vector<float> &query, float threshold, float margin = 0.05f) const;
  std::string encode() const;
  bool decode(const std::string &bytes);
};
} // namespace camera
