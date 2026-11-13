#include "project_identity_json.h"
#include "cJSON.h"
#include <cmath>
namespace camera {
std::string encodeIdentity(const FrameIdentity &f) {
  auto *root = cJSON_CreateObject();
  if (!root)
    return {};
  cJSON_AddNumberToObject(root, "schema_version", 1);
  cJSON_AddStringToObject(root, "device_id", f.device.c_str());
  cJSON_AddStringToObject(root, "event_id", f.event.c_str());
  cJSON_AddNumberToObject(root, "photo_index", f.index);
  cJSON_AddNumberToObject(root, "expected_frames", 5);
  cJSON_AddStringToObject(root, "jpeg_sha256", f.sha256.c_str());
  cJSON_AddNumberToObject(root, "width", f.width);
  cJSON_AddNumberToObject(root, "height", f.height);
  cJSON_AddNullToObject(root, "captured_at");
  cJSON_AddBoolToObject(root, "night_capture", f.night);
  cJSON_AddBoolToObject(root, "supports_exempted", true);
  if (f.recognizer.empty())
    cJSON_AddNullToObject(root, "recognizer_version");
  else
    cJSON_AddStringToObject(root, "recognizer_version", f.recognizer.c_str());
  if (f.database.empty())
    cJSON_AddNullToObject(root, "database_version");
  else
    cJSON_AddStringToObject(root, "database_version", f.database.c_str());
  cJSON_AddStringToObject(root, "frame_identity_status", f.status.c_str());
  auto *faces = cJSON_AddArrayToObject(root, "faces");
  const bool acceptsMatches = (f.status == "complete" || f.status == "partial") &&
                              !f.recognizer.empty() && !f.database.empty();
  for (const auto &face : f.faces) {
    auto *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "face_id", face.id.c_str());
    auto *box = cJSON_AddArrayToObject(o, "bbox");
    for (int coordinate : face.bbox)
      cJSON_AddItemToArray(box, cJSON_CreateNumber(coordinate));
    const bool matched = acceptsMatches && !face.match.member.empty() && face.match.version > 0 &&
                         std::isfinite(face.match.score);
    cJSON_AddStringToObject(o, "status", matched ? "matched" : "unknown");
    if (matched) {
      cJSON_AddStringToObject(o, "member_id", face.match.member.c_str());
      cJSON_AddNumberToObject(o, "member_version", face.match.version);
    } else {
      cJSON_AddNullToObject(o, "member_id");
      cJSON_AddNullToObject(o, "member_version");
    }
    cJSON_AddNumberToObject(o, "match_score", matched ? face.match.score : 0.0);
    cJSON_AddItemToArray(faces, o);
  }
  char *p = cJSON_PrintUnformatted(root);
  std::string out = p ? p : "";
  free(p);
  cJSON_Delete(root);
  return out;
}
} // namespace camera
