#pragma once
#include "project_core.h"
#include <array>
namespace camera {
struct FaceObservation {
  std::string id;
  std::array<int, 4> bbox;
  Match match;
};
struct FrameIdentity {
  std::string device, event, sha256, recognizer, database, status = "not_run";
  int index = 1, width = 0, height = 0;
  bool night = false;
  std::vector<FaceObservation> faces;
};
std::string encodeIdentity(const FrameIdentity &frame);
} // namespace camera
