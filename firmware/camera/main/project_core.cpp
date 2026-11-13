#include "project_core.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
namespace camera {
bool validIdentifier(const std::string &s, size_t max) {
  if (s.empty() || s.size() > max)
    return false;
  for (unsigned char c : s)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
          c == '-'))
      return false;
  return true;
}
bool constantTimeEqual(const std::string &a, const std::string &b) {
  size_t diff = a.size() ^ b.size();
  for (size_t i = 0; i < std::max(a.size(), b.size()); ++i)
    diff |= (i < a.size() ? a[i] : 0) ^ (i < b.size() ? b[i] : 0);
  return diff == 0;
}
AlarmState parseAlarm(const std::string &s) {
  if (s == "pending")
    return AlarmState::Pending;
  if (s == "suspicious")
    return AlarmState::Suspicious;
  if (s == "normal")
    return AlarmState::Normal;
  if (s == "review")
    return AlarmState::Review;
  if (s == "exempted")
    return AlarmState::Exempted;
  return AlarmState::Unknown;
}
int PreviewSession::open(const std::string &r, const std::string &newId, uint64_t now) {
  if (!validIdentifier(r, 64))
    return 400;
  if (valid(now))
    return request == r ? 202 : 409;
  request = r;
  id = newId;
  deadline = now + 30000;
  ++generation;
  return 202;
}
bool PreviewSession::renew(const std::string &sid, uint64_t now) {
  if (sid != id || !valid(now))
    return false;
  deadline = now + 30000;
  return true;
}
bool PreviewSession::close(const std::string &sid) {
  if (sid != id)
    return false;
  id.clear();
  request.clear();
  deadline = 0;
  ++generation;
  return true;
}
static float cosine(const std::vector<float> &a, const std::vector<float> &b) {
  if (a.size() != b.size() || a.empty())
    return -1;
  double ab = 0, aa = 0, bb = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    if (!std::isfinite(a[i]) || !std::isfinite(b[i]))
      return -1;
    ab += a[i] * b[i];
    aa += a[i] * a[i];
    bb += b[i] * b[i];
  }
  return aa > 0 && bb > 0 ? float(ab / std::sqrt(aa * bb)) : -1;
}
bool consistentSample(const std::vector<std::vector<float>> &accepted,
                      const std::vector<float> &candidate, float threshold) {
  if (!std::isfinite(threshold) || threshold <= 0 || threshold > 1 ||
      cosine(candidate, candidate) < 0.99f)
    return false;
  for (const auto &previous : accepted)
    if (cosine(previous, candidate) < threshold)
      return false;
  return true;
}
bool Registry::commit(const std::string &m, uint32_t v, const std::string &cmd,
                      const std::vector<std::vector<float>> &f, float consistencyThreshold) {
  if (!validIdentifier(m) || !validIdentifier(cmd) || v == 0 || f.size() != Samples)
    return false;
  const auto dims = f.front().size();
  if (dims == 0 || dims > MaxDimensions)
    return false;
  for (const auto &x : f)
    if (x.size() != dims || cosine(x, x) < 0.99f)
      return false;
  for (const auto &x : f)
    if (!consistentSample(f, x, consistencyThreshold))
      return false;
  auto it = std::find_if(entries.begin(), entries.end(),
                         [&](const Template &t) { return t.member == m; });
  if (it != entries.end()) {
    if (it->version > v || (it->version == v && (it->command != cmd || it->revoked)))
      return false;
    if (it->version == v)
      return true;
  } else if (entries.size() >= MaxMembers)
    return false;
  Template t{m, cmd, v, version + 1, false, f};
  if (it == entries.end())
    entries.push_back(t);
  else
    *it = t;
  ++version;
  return true;
}
bool Registry::revoke(const std::string &m, uint32_t v, const std::string &cmd) {
  if (!validIdentifier(m) || !validIdentifier(cmd) || v == 0)
    return false;
  auto it = std::find_if(entries.begin(), entries.end(),
                         [&](const Template &t) { return t.member == m; });
  if (it != entries.end()) {
    if (it->version > v)
      return false;
    if (it->version == v && it->revoked && it->command == cmd)
      return true;
  } else if (entries.size() >= MaxMembers)
    return false;
  Template t{m, cmd, v, version + 1, true, {}};
  if (it == entries.end())
    entries.push_back(t);
  else
    *it = t;
  ++version;
  return true;
}
Match Registry::match(const std::vector<float> &q, float threshold, float margin) const {
  Match best;
  float second = -1;
  for (const auto &t : entries) {
    if (t.revoked)
      continue;
    float score = -1;
    for (const auto &f : t.features)
      score = std::max(score, cosine(q, f));
    if (score > best.score) {
      second = best.score;
      best = {t.member, t.version, score};
    } else
      second = std::max(second, score);
  }
  if (best.score < threshold || best.score - second < margin)
    return {};
  return best;
}
// Versioned binary record with CRC. A full replacement is persisted atomically by FaceStore.
static uint32_t crc(const std::string &s) {
  uint32_t c = 0xffffffff;
  for (unsigned char x : s) {
    c ^= x;
    for (int i = 0; i < 8; ++i)
      c = (c >> 1) ^ ((c & 1) ? 0xedb88320 : 0);
  }
  return ~c;
}
static void put(std::string &o, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    o.push_back(char((v >> (8 * i)) & 255));
}
static void str(std::string &o, const std::string &s) {
  put(o, uint32_t(s.size()));
  o += s;
}
std::string Registry::encode() const {
  std::string o = "FREG2:MFN_S8_V1";
  put(o, version);
  put(o, uint32_t(entries.size()));
  for (const auto &t : entries) {
    str(o, t.member);
    str(o, t.command);
    put(o, t.version);
    put(o, t.databaseVersion);
    put(o, t.revoked ? 1 : 0);
    put(o, uint32_t(t.features.size()));
    for (const auto &f : t.features) {
      put(o, uint32_t(f.size()));
      for (float x : f) {
        uint32_t b;
        std::memcpy(&b, &x, 4);
        put(o, b);
      }
    }
  }
  put(o, crc(o));
  return o;
}
bool Registry::decode(const std::string &s) {
  Registry parsed;
  size_t p = 15;
  bool ok = true;
  auto get = [&]() {
    uint32_t v = 0;
    if (p + 4 > s.size()) {
      ok = false;
      return v;
    }
    for (int i = 0; i < 4; ++i)
      v |= uint32_t(uint8_t(s[p++])) << (i * 8);
    return v;
  };
  auto read = [&]() {
    auto n = get();
    if (n > 128 || p + n > s.size()) {
      ok = false;
      return std::string();
    }
    auto r = s.substr(p, n);
    p += n;
    return r;
  };
  if (s.size() < 27 || s.substr(0, 15) != "FREG2:MFN_S8_V1")
    return false;
  parsed.version = get();
  auto count = get();
  if (count > MaxMembers)
    return false;
  for (uint32_t i = 0; i < count && ok; ++i) {
    Template t;
    t.member = read();
    t.command = read();
    t.version = get();
    t.databaseVersion = get();
    t.revoked = get() != 0;
    auto n = get();
    if (!validIdentifier(t.member) || !validIdentifier(t.command) || !t.version ||
        !t.databaseVersion || t.databaseVersion > parsed.version || n > (t.revoked ? 0 : Samples) ||
        (!t.revoked && n != Samples))
      return false;
    for (uint32_t j = 0; j < n && ok; ++j) {
      auto dims = get();
      if (dims == 0 || dims > MaxDimensions)
        return false;
      std::vector<float> f;
      for (uint32_t k = 0; k < dims && ok; ++k) {
        uint32_t b = get();
        float x;
        std::memcpy(&x, &b, 4);
        f.push_back(x);
      }
      if (cosine(f, f) < 0.99f)
        return false;
      t.features.push_back(f);
    }
    for (const auto &e : parsed.entries)
      if (e.member == t.member)
        return false;
    parsed.entries.push_back(t);
  }
  if (!ok || p + 4 != s.size())
    return false;
  auto checksum = get();
  if (checksum != crc(s.substr(0, s.size() - 4)))
    return false;
  *this = std::move(parsed);
  return true;
}
} // namespace camera
