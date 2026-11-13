#include "../main/project_identity_json.h"
#include "cJSON.h"
#include "test_check.h"
#define assert CHECK
#include <iostream>
int main(int argc, char **argv) {
  using namespace camera;
  FrameIdentity f;
  f.device = "contract_camera";
  f.event = "contract_event";
  f.width = 320;
  f.height = 240;
  f.sha256 = argc > 1 ? argv[1] : std::string(64, '0');
  if (argc > 3) {
    f.width = std::stoi(argv[2]);
    f.height = std::stoi(argv[3]);
  }
  f.recognizer = "esp-dl-MFN_S8_V1-0.3.3";
  f.database = "3";
  f.status = "complete";
  f.faces.push_back({"f1", {10, 20, 70, 90}, {}});
  auto unknown = encodeIdentity(f);
  auto *parsed = cJSON_Parse(unknown.c_str());
  auto *face = cJSON_GetArrayItem(cJSON_GetObjectItem(parsed, "faces"), 0);
  assert(cJSON_GetArraySize(face) == 6);
  assert(cJSON_IsNull(cJSON_GetObjectItem(face, "member_id")));
  assert(cJSON_IsNull(cJSON_GetObjectItem(face, "member_version")));
  assert(cJSON_IsNumber(cJSON_GetObjectItem(face, "match_score")));
  cJSON_Delete(parsed);
  std::cout << unknown << '\n';
  f.faces[0].match = {"member_alice", 2, 0.9f};
  std::cout << encodeIdentity(f) << '\n';
  for (const char *state : {"timeout", "error", "not_run"}) {
    f.status = state;
    auto json = encodeIdentity(f);
    parsed = cJSON_Parse(json.c_str());
    face = cJSON_GetArrayItem(cJSON_GetObjectItem(parsed, "faces"), 0);
    assert(std::string(cJSON_GetObjectItem(face, "status")->valuestring) == "unknown");
    assert(cJSON_IsNull(cJSON_GetObjectItem(face, "member_id")));
    cJSON_Delete(parsed);
    std::cout << json << '\n';
  }
  f.faces.clear();
  f.status = "complete";
  std::cout << encodeIdentity(f) << '\n';
}
