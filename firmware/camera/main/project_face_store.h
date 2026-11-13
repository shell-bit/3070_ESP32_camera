#pragma once
#include "project_core.h"
class FaceStore {
  bool mounted = false;

public:
  camera::Registry registry;
  bool begin();
  bool save(const camera::Registry &next);
  bool ready() const {
    return mounted;
  }
};
