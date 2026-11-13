#include "project_face_identity.h"
#include "project_settings.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "mbedtls/sha256.h"
#include "project_identity_json.h"
#include "jpeg_decoder.h"
#include <algorithm>
#include <cmath>
#if PROJECT_FACE_ENABLED
#include "human_face_detect.hpp"
#include "human_face_recognition.hpp"
#endif
namespace {
FaceStore store;
bool ready = false;
#if PROJECT_FACE_ENABLED
HumanFaceDetect *detector = nullptr;
HumanFaceFeat *features = nullptr;
struct Decoded {
  std::shared_ptr<uint8_t> data;
  uint16_t width = 0, height = 0;
};
Decoded decode(const OwnedJpeg &jpeg) {
  Decoded out;
  esp_jpeg_image_cfg_t c{};
  c.indata = jpeg.bytes.get();
  c.indata_size = jpeg.size;
  c.out_format = JPEG_IMAGE_FORMAT_RGB888;
  c.out_scale = jpeg.width > 1280  ? JPEG_IMAGE_SCALE_1_4
                : jpeg.width > 640 ? JPEG_IMAGE_SCALE_1_2
                                   : JPEG_IMAGE_SCALE_0;
  esp_jpeg_image_output_t info{};
  if (esp_jpeg_get_image_info(&c, &info) != ESP_OK || info.output_len > 640 * 480 * 3)
    return out;
  auto *p = (uint8_t *)heap_caps_malloc(info.output_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p)
    return out;
  out.data = std::shared_ptr<uint8_t>(p, free);
  c.outbuf = p;
  c.outbuf_size = info.output_len;
  if (esp_jpeg_decode(&c, &info) != ESP_OK)
    return {};
  out.width = info.width;
  out.height = info.height;
  return out;
}
std::vector<float> feature(const dl::image::img_t &img, const dl::detect::result_t &face) {
  auto *t = features->run(img, face.keypoint);
  if (!t || t->dtype != dl::DATA_TYPE_FLOAT || t->get_size() < 1 || t->get_size() > 1024)
    return {};
  auto *p = t->get_element_ptr<float>();
  return p ? std::vector<float>(p, p + t->get_size()) : std::vector<float>();
}
bool sharp(const dl::image::img_t &img, const dl::detect::result_t &f) {
  if (f.box.size() != 4 || f.keypoint.size() < 10 || f.score < 0.8f || f.box[2] - f.box[0] < 40 ||
      f.box[3] - f.box[1] < 40)
    return false;
  auto *p = (uint8_t *)img.data;
  double energy = 0;
  int samples = 0;
  for (int y = std::max(1, f.box[1]); y < std::min(int(img.height) - 1, f.box[3]); y += 3)
    for (int x = std::max(1, f.box[0]); x < std::min(int(img.width) - 1, f.box[2]); x += 3) {
      int i = (y * img.width + x) * 3;
      int d = int(p[i]) - int(p[i + 3]);
      energy += d * d;
      ++samples;
    }
  return samples > 0 && energy / samples > 30;
}
#endif
} // namespace
FaceStore &faceStore() {
  return store;
}
const char *faceRecognizerVersion() {
  return "esp-dl-MFN_S8_V1-0.3.3";
}
String faceDatabaseVersion() {
  return String(store.registry.version);
}
bool faceIdentityReady() {
  return ready;
}
void setupFaceIdentity() {
#if PROJECT_FACE_ENABLED
  if (!store.begin()) {
    Serial.println(
        "Face store unavailable; initialize dedicated FATFS partition before enrollment.");
    return;
  }
  detector = new HumanFaceDetect(HumanFaceDetect::MSRMNP_S8_V1, false);
  features = new HumanFaceFeat(HumanFaceFeat::MFN_S8_V1, false);
  ready = detector && features;
#else
  Serial.println(
      "Face recognition disabled in this build; identity is not_run and no exemption is inferred.");
#endif
}
bool extractEnrollmentFeature(const OwnedJpeg &jpeg, std::vector<float> &out, float &pose) {
#if PROJECT_FACE_ENABLED
  if (!ready)
    return false;
  auto d = decode(jpeg);
  if (!d.data)
    return false;
  dl::image::img_t img{d.data.get(), d.width, d.height, dl::image::DL_IMAGE_PIX_TYPE_RGB888};
  auto faces = detector->run(img);
  if (faces.size() != 1 || !sharp(img, faces.front()))
    return false;
  auto &face = faces.front();
  out = feature(img, face);
  float eyes = float(face.keypoint[2] - face.keypoint[0]);
  if (std::abs(eyes) < 4)
    return false;
  pose = (face.keypoint[4] - (face.keypoint[0] + face.keypoint[2]) * 0.5f) / std::abs(eyes);
  return !out.empty();
#else
  (void)jpeg;
  (void)out;
  (void)pose;
  return false;
#endif
}
String identifyEventFrame(const OwnedJpeg &jpeg, const String &event, int index, bool night,
                          uint64_t deadline) {
  camera::FrameIdentity metadata;
  metadata.device = DEVICE_ID;
  metadata.event = event.c_str();
  metadata.index = index;
  metadata.width = jpeg.width;
  metadata.height = jpeg.height;
  metadata.night = night;
  uint8_t digest[32];
  mbedtls_sha256(jpeg.bytes.get(), jpeg.size, digest, 0);
  char hex[65];
  for (int i = 0; i < 32; ++i)
    sprintf(hex + i * 2, "%02x", digest[i]);
  metadata.sha256 = hex;
  if (ready) {
    metadata.recognizer = faceRecognizerVersion();
    metadata.database = faceDatabaseVersion().c_str();
  }
#if PROJECT_FACE_ENABLED
  if (ready) {
    metadata.status = "timeout";
    if (uint64_t(esp_timer_get_time() / 1000) < deadline) {
      auto d = decode(jpeg);
      if (!d.data)
        metadata.status = "error";
      else {
        dl::image::img_t img{d.data.get(), d.width, d.height, dl::image::DL_IMAGE_PIX_TYPE_RGB888};
        auto faces = detector->run(img);
        metadata.status = faces.size() > 8 ? "partial" : "complete";
        int n = 0;
        for (const auto &f : faces) {
          if (n >= 8)
            break;
          if (uint64_t(esp_timer_get_time() / 1000) >= deadline) {
            metadata.status = "timeout";
            break;
          }
          if (f.box.size() != 4 || f.keypoint.size() < 10) {
            metadata.status = "partial";
            continue;
          }
          camera::FaceObservation observation;
          observation.id = "f" + std::to_string(n + 1);
          for (int k = 0; k < 4; ++k) {
            int extent = k % 2 ? jpeg.height : jpeg.width, scaled = k % 2 ? d.height : d.width;
            observation.bbox[k] =
                std::max(0, std::min(extent, int(int64_t(f.box[k]) * extent / scaled)));
          }
          if (observation.bbox[0] >= observation.bbox[2] ||
              observation.bbox[1] >= observation.bbox[3]) {
            metadata.status = "partial";
            continue;
          }
          auto q = feature(img, f);
          observation.match = store.registry.match(q, 0.70f);
          if (uint64_t(esp_timer_get_time() / 1000) >= deadline) {
            metadata.status = "timeout";
            break;
          }
          metadata.faces.push_back(std::move(observation));
          ++n;
        }
        if (uint64_t(esp_timer_get_time() / 1000) >= deadline)
          metadata.status = "timeout";
      }
    }
  }
#else
  (void)deadline;
#endif
  // The shared serializer always emits all six face fields and strips matches
  // for timeout/error/not_run, including results completed earlier in this frame.
  return String(camera::encodeIdentity(metadata).c_str());
}
