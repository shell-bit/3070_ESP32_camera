#pragma once
#include "project_camera_manager.h"
#include "project_face_store.h"
void setupFaceIdentity();
bool faceIdentityReady();
const char *faceRecognizerVersion();
String faceDatabaseVersion();
FaceStore &faceStore();
bool extractEnrollmentFeature(const OwnedJpeg &jpeg, std::vector<float> &feature, float &pose);
String identifyEventFrame(const OwnedJpeg &jpeg, const String &event, int index, bool night,
                          uint64_t deadline);
