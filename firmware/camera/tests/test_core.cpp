#include "../main/project_core.h"
#include "test_check.h"
#define assert CHECK
#include <iostream>
int main() {
  using namespace camera;
  PreviewSession s;
  assert(s.open("r1", "s1", 100) == 202);
  assert(s.open("r1", "s2", 200) == 202 && s.id == "s1");
  assert(s.open("other", "s2", 200) == 409);
  assert(s.valid(30099));
  assert(!s.valid(30100));
  assert(!s.renew("s1", 30100));
  assert(s.open("r2", "s2", 31000) == 202);
  assert(!s.close("s1") && s.valid(31001));
  assert(s.renew("s2", 41000));
  assert(s.valid(70999));
  assert(!s.valid(71000));
  assert(!validIdentifier("a/../b") && validIdentifier("family_1-2"));
  assert(constantTimeEqual("abcd", "abcd") && !constantTimeEqual("abcd", "abc"));
  assert(parseAlarm("exempted") == AlarmState::Exempted);
  assert(parseAlarm("unexpected") == AlarmState::Unknown);
  assert(parseAlarm("suspicious") == AlarmState::Suspicious);
  Budget b(100, 10000);
  assert(b.available(10099));
  assert(!b.available(10100));
  assert(b.remaining(10101) == 0 && b.remaining(101) == 9999);
  Registry r;
  std::vector<std::vector<float>> a{{1, 0}, {0.99f, 0.01f}, {1, 0.01f}};
  assert(r.commit("alice", 2, "c2", a));
  assert(r.match({1, 0}, 0.8f).member == "alice");
  assert(!r.commit("alice", 1, "old", a));
  assert(r.revoke("alice", 3, "r3"));
  assert(r.match({1, 0}, 0.8f).member.empty());
  assert(!r.commit("alice", 2, "c2", a));
  assert(r.commit("alice", 4, "c4", a));
  assert(!r.commit("bob", 1, "b1", {{1, 0}}));
  assert(!r.commit("bob", 1, "b1", {{1, 0}, {1, 0}, {0, 0}}));
  assert(!r.commit("mixed", 1, "mixed_people", {{1, 0}, {0, 1}, {1, 0}}));
  auto encoded = r.encode();
  Registry loaded;
  assert(loaded.decode(encoded));
  assert(loaded.match({1, 0}, 0.8f).member == "alice");
  encoded[encoded.size() / 2] ^= 1;
  Registry corrupt;
  assert(!corrupt.decode(encoded));
  assert(corrupt.match({1, 0}, 0.8f).member.empty());
  assert(r.commit("bob", 1, "b1", a));
  assert(r.match({1, 0}, 0.8f).member.empty()); // ambiguous identities never accepted
  Registry rebooted;
  assert(rebooted.decode(r.encode()));
  assert(rebooted.entries[0].command == "c4" && rebooted.entries[0].databaseVersion == 3);
  assert(rebooted.entries[1].command == "b1" && rebooted.entries[1].databaseVersion == 4);
  assert(rebooted.revoke("alice", 5, "revoke_attempt_1"));
  assert(rebooted.revoke("alice", 5, "revoke_attempt_2"));
  Registry rebootedAgain;
  assert(rebootedAgain.decode(rebooted.encode()));
  assert(rebootedAgain.entries[0].command == "revoke_attempt_2");
  std::cout << "camera core: session, identity, revocation, corruption and budget checks passed\n";
}
