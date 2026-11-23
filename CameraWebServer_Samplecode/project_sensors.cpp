#include <Arduino.h>
#include <algorithm>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "esp_camera.h"
#include "img_converters.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp32s3/rom/tjpgd.h"
#include "project_settings.h"
#include "project_sensors.h"
#include "project_sleep.h"
#include "project_ai_alarm.h"
#include "project_audio.h"
#include "project_transport.h"

namespace {
struct Photo { uint8_t *data = nullptr; size_t size = 0; };
Photo pending[BURST_COUNT];
String eventId;
unsigned long lastRetry = 0;
unsigned long uploadStarted = 0;
unsigned long uploadBudget = UPLOAD_GIVE_UP_MS;
unsigned long networkPreparationStarted = 0;
bool uploadWindowStarted = false;
unsigned long uploadTimeRemaining();
class UploadTlsClient : public WiFiClientSecure {
 public:
  unsigned long firstWrite = 0, lastWrite = 0;
  bool wrote = false;
  size_t write(const uint8_t *data, size_t size) override {
    if (!uploadTimeRemaining()) {
      stop();
      return 0;
    }
    if (!wrote) { firstWrite = millis(); wrote = true; }
    // The ESP32 TLS driver uses 4 KiB records internally. Returning at each
    // record also lets HTTPClient abandon a slow-but-progressing body at our cap.
    size_t sent = WiFiClientSecure::write(data, std::min(size, size_t(4096)));
    lastWrite = millis();
    return sent;
  }
};
UploadTlsClient photoClient;
HTTPClient photoHttp;
bool photoTlsConfigured = false;
bool tryBurstUpload = true;
bool acceptsDeflateAudio = false;
unsigned burstAttempts = 0;
bool nightEvent = false;
const size_t MAX_PHOTO_BYTES = 1024 * 1024;
constexpr BYTE JPEG_CHECK_SCALE = 3; // Decode every MCU, discard its 1/8-size output.
// The capture loop is the only caller. No full RGB image is allocated.
alignas(4) uint8_t jpegWorkspace[4096];

struct JpegInput {
  const uint8_t *data = nullptr;
  size_t size = 0;
  size_t offset = 0;
  size_t pixels = 0;
  unsigned blocks = 0;
};

UINT readJpeg(JDEC *decoder, BYTE *buffer, UINT requested) {
  JpegInput *input = static_cast<JpegInput *>(decoder->device);
  size_t count = requested;
  if (count > input->size - input->offset) count = input->size - input->offset;
  if (buffer && count) memcpy(buffer, input->data + input->offset, count);
  input->offset += count;
  return static_cast<UINT>(count);
}

UINT discardJpegPixels(JDEC *decoder, void *, JRECT *area) {
  JpegInput *input = static_cast<JpegInput *>(decoder->device);
  const unsigned width = decoder->width >> JPEG_CHECK_SCALE;
  const unsigned height = decoder->height >> JPEG_CHECK_SCALE;
  if (area->left > area->right || area->top > area->bottom ||
      area->right >= width || area->bottom >= height) return 0;
  input->pixels += size_t(area->right - area->left + 1) * (area->bottom - area->top + 1);
  if (input->pixels > size_t(width) * height) return 0;
  // Let the microphone task run while checking a large JPEG.
  if (++input->blocks % 128 == 0) delay(1);
  return 1;
}

const char *checkJpeg(const uint8_t *data, size_t size, const resolution_info_t &expected, int &decoderResult) {
  decoderResult = -1;
  if (!data || size < 4 || size > MAX_PHOTO_BYTES) return "invalid JPEG byte count";
  if (data[0] != 0xff || data[1] != 0xd8 || data[2] != 0xff)
    return "missing JPEG start marker";
  if (data[size - 2] != 0xff || data[size - 1] != 0xd9)
    return "missing JPEG end marker";

  JpegInput input;
  input.data = data;
  input.size = size;
  JDEC decoder = {};
  JRESULT result = jd_prepare(&decoder, readJpeg, jpegWorkspace, sizeof(jpegWorkspace), &input);
  decoderResult = int(result);
  if (result != JDR_OK) return "JPEG header decoder rejected the image";
  if (decoder.width != expected.width || decoder.height != expected.height)
    return "JPEG header dimensions differ from the configured resolution";
  // Scaling reduces output work, but the decoder still reads the entropy data
  // for every MCU. Input size, image dimensions and workspace are all bounded.
  result = jd_decomp(&decoder, discardJpegPixels, JPEG_CHECK_SCALE);
  decoderResult = int(result);
  if (result != JDR_OK) return "JPEG image decoder rejected the image";
  if (input.pixels != size_t(expected.width >> JPEG_CHECK_SCALE) * (expected.height >> JPEG_CHECK_SCALE))
    return "JPEG decoder did not finish the image";
  return nullptr;
}

struct EncodedFrame {
  uint8_t *data;
  size_t capacity;
  size_t used;
  bool failed;
};

size_t appendJpeg(void *context, size_t index, const void *data, size_t count) {
  EncodedFrame &out = *static_cast<EncodedFrame *>(context);
  if (!count) return 0; // Encoder end-of-image callback.
  if (out.failed || !data || index != out.used || count > out.capacity - out.used) {
    out.failed = true;
    return 0;
  }
  memcpy(out.data + index, data, count);
  out.used += count;
  return count;
}

const char *encodeRgbFrame(camera_fb_t *fb, uint8_t *&jpeg, size_t &bytes) {
  const size_t rawBytes = size_t(fb->width) * fb->height * 2;
  if (fb->len != rawBytes) return "short RGB565 frame from camera";
  EncodedFrame out = {nullptr, std::min(rawBytes, MAX_PHOTO_BYTES), 0, false};
  jpeg = (uint8_t *)heap_caps_malloc(out.capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!jpeg) return "insufficient PSRAM for JPEG encoding";
  out.data = jpeg;
  if (!frame2jpg_cb(fb, OV3660_SOFTWARE_JPEG_QUALITY, appendJpeg, &out) || out.failed || !out.used)
    return "software JPEG encoding failed";
  bytes = out.used;
  // Free unused PSRAM before retaining the image for the rest of the burst.
  uint8_t *compact = (uint8_t *)heap_caps_realloc(jpeg, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (compact) jpeg = compact;
  return nullptr;
}

unsigned long uploadTimeRemaining() {
  if (!uploadWindowStarted) return uploadBudget;
  unsigned long elapsed = millis() - uploadStarted;
  return elapsed < uploadBudget ? uploadBudget - elapsed : 0;
}

void closePhotoUpload() {
  if (!photoTlsConfigured) return;
  photoHttp.setReuse(false);
  photoHttp.end();
  photoClient.stop();
  photoTlsConfigured = false;
}

// Use HTTPClient's response decoder with a bounded sink, rather than allocating
// an arbitrary proxy error page. Only completely read, length-delimited responses
// are reused; chunked or close-delimited replies get a fresh connection next time.
constexpr size_t MAX_UPLOAD_RESPONSE = 2048;
class UploadResponse : public Stream {
 public:
  char body[MAX_UPLOAD_RESPONSE + 1] = {};
  size_t size = 0;
  unsigned long started = millis();
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
  size_t write(uint8_t value) override { return write(&value, 1); }
  size_t write(const uint8_t *data, size_t count) override {
    if (!uploadTimeRemaining() || millis() - started >= UPLOAD_RESPONSE_TIMEOUT_MS ||
        count > MAX_UPLOAD_RESPONSE - size) {
      // Also terminate a stalled read that HTTPClient reports as a zero-byte write.
      photoClient.stop();
      return 0;
    }
    memcpy(body + size, data, count);
    size += count;
    body[size] = '\0';
    return count;
  }
};

bool readUploadResponse(UploadResponse &response, bool &canReuse) {
  int length = photoHttp.getSize();
  if (length > int(MAX_UPLOAD_RESPONSE)) return false;
  String connection = photoHttp.header("Connection");
  connection.toLowerCase();
  String transfer = photoHttp.header("Transfer-Encoding");
  transfer.toLowerCase();
  transfer.trim();
  // An unframed body can wait indefinitely for a peer that never closes.
  if (length < 0 && transfer != "chunked") return false;
  canReuse = length >= 0 && (transfer.isEmpty() || transfer == "identity") &&
             connection.indexOf("close") < 0;
  int received = photoHttp.writeToStream(&response);
  return received >= 0 && size_t(received) == response.size &&
         (length < 0 || size_t(length) == response.size);
}

bool canReuseEmptyResponse() {
  String connection = photoHttp.header("Connection");
  connection.toLowerCase();
  return connection.indexOf("close") < 0 && photoHttp.getSize() <= 0 &&
         photoHttp.header("Transfer-Encoding").isEmpty();
}

bool prepareUpload(const String &url) {
  if (!photoTlsConfigured) {
    if (!configureProjectTls(photoClient)) return false;
    photoTlsConfigured = true;
  }
  if (!uploadWindowStarted) {
    size_t bytes = projectAudioPendingBytes();
    for (auto &photo : pending) bytes += photo.size;
    // Slow measured transfers take ~3 KiB/s. Include the WAV, plus connection
    // and acknowledgement time, while keeping a bounded PIR wake cycle.
    unsigned long estimated = 20000UL + ((bytes + 2559UL) / 2560UL) * 1000UL;
    uploadBudget = constrain(estimated, UPLOAD_GIVE_UP_MS, 300000UL);
    uploadStarted = millis();
    uploadWindowStarted = true;
    Serial.printf("Upload retry window: %lu seconds for %u bytes; Wi-Fi and clock are ready.\n",
                  uploadBudget / 1000, unsigned(bytes));
  }
  unsigned long remaining = uploadTimeRemaining();
  if (!remaining) return false;
  photoHttp.setReuse(true);
  // Never forward the camera credential to a redirected host.
  photoHttp.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  photoHttp.setConnectTimeout(remaining < UPLOAD_CONNECT_TIMEOUT_MS ? remaining : UPLOAD_CONNECT_TIMEOUT_MS);
  photoHttp.setTimeout(remaining < UPLOAD_RESPONSE_TIMEOUT_MS ? remaining : UPLOAD_RESPONSE_TIMEOUT_MS);
  unsigned long handshakeSeconds = (remaining + 999) / 1000;
  photoClient.setHandshakeTimeout(handshakeSeconds < 12 ? handshakeSeconds : 12);
  if (!photoHttp.begin(photoClient, url)) {
    closePhotoUpload();
    return false;
  }
  const char *headers[] = {"Connection", "Transfer-Encoding", "Preference-Applied", "X-Burst-Received", "X-Event-Audio-Encoding"};
  photoHttp.collectHeaders(headers, sizeof(headers)/sizeof(headers[0]));
  return true;
}

void light(bool on) {
  if (WHITE_LED_PIN >= 0)
    digitalWrite(WHITE_LED_PIN, on ? WHITE_LED_ACTIVE_LEVEL : !WHITE_LED_ACTIVE_LEVEL);
}
bool hasPendingPhotos() {
  for (auto &photo : pending) if (photo.data) return true;
  return false;
}
bool hasPending() { return hasPendingPhotos() || projectAudioRecording() || projectAudioPending(); }
void discardPending(const char *reason) {
  closePhotoUpload();
  discardProjectAudio();
  int dropped = 0;
  for (auto &photo : pending) {
    if (photo.data) { free(photo.data); photo = Photo(); ++dropped; }
  }
  if (dropped) Serial.printf("Discarded %d unsent photos: %s. Ready for next trigger.\n", dropped, reason);
}

// If PSRAM is fragmented, HTTPClient can stream the original buffers directly.
// Its stream overload copies up to one TCP segment at a time. The contiguous
// fast path below lets the TLS library use larger records when memory permits.
class BurstStream : public Stream {
 public:
  explicit BurstStream(size_t total) : remaining(total) {}
  int available() override { return int(remaining); }
  int peek() override {
    while (index < BURST_COUNT && offset == pending[index].size) { ++index; offset = 0; }
    return index < BURST_COUNT ? pending[index].data[offset] : -1;
  }
  int read() override {
    uint8_t value;
    return readBytes(&value, 1) == 1 ? value : -1;
  }
  size_t readBytes(uint8_t *dest, size_t length) override {
    size_t count = 0;
    while (count < length && peek() >= 0) {
      size_t piece = std::min(length-count, pending[index].size-offset);
      memcpy(dest+count, pending[index].data+offset, piece);
      offset += piece;
      count += piece;
      remaining -= piece;
    }
    return count;
  }
  size_t write(uint8_t) override { return 0; }
 private:
  size_t remaining, offset = 0;
  int index = 0;
};

void uploadBurst() {
  // The manifest keeps capture positions, including slots rejected by the camera.
  String lengths;
  size_t bytes = 0;
  unsigned count = 0;
  for (auto &photo : pending) {
    if (lengths.length()) lengths += ',';
    lengths += String(unsigned(photo.size));
    bytes += photo.size;
    if (photo.data) ++count;
  }
  uint8_t *body = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (body) {
    size_t offset = 0;
    for (auto &photo : pending) {
      if (!photo.data) continue;
      memcpy(body + offset, photo.data, photo.size);
      offset += photo.size;
    }
  }
  addProjectDeviceHeaders(photoHttp);
  photoHttp.addHeader("Content-Type", "application/vnd.ee3070.jpeg-burst");
  photoHttp.addHeader("X-Event-ID", eventId);
  photoHttp.addHeader("X-Expected-Photos", String(BURST_COUNT));
  photoHttp.addHeader("X-Frame-Lengths", lengths);
  photoHttp.addHeader("X-Night-Capture", nightEvent ? "true" : "false");
  unsigned long start = millis();
  ++burstAttempts;
  photoClient.wrote = false;
  BurstStream source(bytes);
  int code = body ? photoHttp.POST(body, bytes) : photoHttp.sendRequest("POST", &source, bytes);
  unsigned long finished = millis();
  free(body);
  logProjectTransportError(photoClient, code);
  bool saved = code == 204 && photoHttp.header("X-Burst-Received") == eventId;
  if(saved) acceptsDeflateAudio = photoHttp.header("X-Event-Audio-Encoding").equalsIgnoreCase("deflate");
  if (saved && canReuseEmptyResponse()) photoHttp.end();
  else closePhotoUpload();
  Serial.printf("Burst upload: HTTP %d; %u/%d photos, %u bytes in %lu ms.\n",
                code, count, BURST_COUNT, unsigned(bytes), finished - start);
  if (photoClient.wrote) {
    Serial.printf("Burst timing: connect %lu ms, request writes %lu ms, response %lu ms.\n",
                  photoClient.firstWrite-start, photoClient.lastWrite-photoClient.firstWrite,
                  finished-photoClient.lastWrite);
  }
  if (saved) {
    Serial.printf("Photos accepted %lu ms after capture (%lu KiB/s for the photo request).\n",
                  finished-networkPreparationStarted,
                  (unsigned long)((uint64_t(bytes)*1000)/(std::max(1UL,finished-start)*1024ULL)));
    for (auto &photo : pending) {
      free(photo.data);
      photo = Photo();
    }
  } else if (code == 401 || code == 403) {
    discardPending("camera credentials rejected; fix the token before retrying");
  } else {
    bool transient = code < 0 || code == 408 || code == 425 || code == 429 ||
                     code >= 500 || (code >= 200 && code < 300);
    unsigned long retryEstimate = std::max(finished - start, UPLOAD_CONNECT_TIMEOUT_MS);
    if (transient && burstAttempts == 1 &&
        uploadTimeRemaining() > retryEstimate + UPLOAD_RETRY_MS) {
      Serial.println("Burst unconfirmed. Retrying the same event and manifest once.");
      lastRetry = millis();
      return;
    }
    // Old servers return 415. A lost acknowledgement can also come from a
    // committed upload: identical event/index/bytes make the fallback idempotent.
    tryBurstUpload = false;
    Serial.println("Burst unconfirmed. Retaining originals for individual upload.");
  }
  lastRetry = millis() - UPLOAD_RETRY_MS;
}

void uploadPending() {
  lastRetry = millis();
  if (uploadWindowStarted && !uploadTimeRemaining()) {
    discardPending("upload retry window expired");
    return;
  }
  if (!LOCAL_DASHBOARD_UPLOAD_URL[0] || WiFi.status() != WL_CONNECTED) {
    closePhotoUpload();
    Serial.println("Photos waiting in RAM: check dashboard URL/Wi-Fi. Will give up after retry window.");
    return;
  }
  if (!projectServerUrl("").length()) {
    discardPending("upload requires a verified HTTPS URL ending in /api/device/upload");
    return;
  }
  if (tryBurstUpload && hasPendingPhotos()) {
    if (prepareUpload(LOCAL_DASHBOARD_UPLOAD_URL)) uploadBurst();
    return;
  }
  for (int i = 0; i < BURST_COUNT; ++i) {
    if (!pending[i].data) continue;
    if (uploadWindowStarted && !uploadTimeRemaining()) {
      discardPending("upload retry window expired");
      break;
    }

    if (!prepareUpload(LOCAL_DASHBOARD_UPLOAD_URL)) return;
    bool reused = photoClient.connected();
    addProjectDeviceHeaders(photoHttp);
    photoHttp.addHeader("Content-Type", "image/jpeg");
    photoHttp.addHeader("Accept", "application/json");
    photoHttp.addHeader("Prefer", "return=minimal");
    photoHttp.addHeader("X-Event-ID", eventId);
    photoHttp.addHeader("X-Photo-Index", String(i + 1));
    // Keep missing/rejected frames visible to the server as incomplete evidence.
    photoHttp.addHeader("X-Expected-Photos", String(BURST_COUNT));
    photoHttp.addHeader("X-Night-Capture", nightEvent ? "true" : "false");
    
    unsigned long requestStarted = millis();
    int code = photoHttp.POST(pending[i].data, pending[i].size);
    logProjectTransportError(photoClient, code);
    UploadResponse response;
    bool canReuse = false;
    bool complete = code == 204;
    bool confirmed = false;
    if (code == 204) {
      // The server sends this only after commit (or a verified idempotent retry).
      // HTTP 204 has no body, so no proxy chunk framing needs to be consumed.
      String applied = photoHttp.header("Preference-Applied");
      applied.toLowerCase();
      applied.trim();
      confirmed = applied == "return=minimal";
      canReuse = canReuseEmptyResponse();
    } else if (code > 0) {
      complete = readUploadResponse(response, canReuse);
      if (complete && (code == 200 || code == 201)) {
        // Compatibility with servers that still return their saved:true JSON.
        JsonDocument acknowledgement;
        DeserializationError error = deserializeJson(acknowledgement, response.body, response.size);
        confirmed = !error && acknowledgement["saved"].is<bool>() && acknowledgement["saved"].as<bool>();
      }
    }
    Serial.printf("Photo %d/%d: dashboard HTTP %d; %u bytes in %lu ms; TLS %s.\n",
                  i + 1, BURST_COUNT, code, unsigned(pending[i].size), millis() - requestStarted,
                  reused ? "reused" : "new connection");
    if (confirmed && canReuse) photoHttp.end();
    else closePhotoUpload();
    if (confirmed) {
      free(pending[i].data);
      pending[i] = Photo();
      lastRetry = millis() - UPLOAD_RETRY_MS;
    } else {
      if (code == 401 || code == 403) {
        Serial.println("Upload rejected: check DEVICE_ID and DEVICE_UPLOAD_TOKEN.");
        discardPending("camera credentials rejected; fix the token before retrying");
        return;
      }
      if (!complete && code > 0)
        Serial.println("Upload response incomplete/oversized; retrying unchanged event/photo on a fresh connection.");
      else if (code == 200 || code == 201 || code == 204)
        Serial.println("Upload response did not confirm storage; retaining the photo.");
      Serial.println("Upload not confirmed; retained for retry. Check dashboard URL/server/firewall.");
      lastRetry = millis();
    }
    // Send one photo per loop so the retry deadline is checked between requests.
    return;
  }
  if (projectAudioRecording()) {
    // Leave the verified connection open for the remaining part of the clip.
    lastRetry = millis() - UPLOAD_RETRY_MS;
    return;
  }
  if (projectAudioPending() && prepareUpload(projectServerUrl("/api/device/audio")))
    uploadProjectAudio(eventId, photoHttp, photoClient, acceptsDeflateAudio);
  closePhotoUpload(); // The intercom uses its own WebSocket connection.
}
void captureBurst() {
  tryBurstUpload = true;
  acceptsDeflateAudio = false;
  burstAttempts = 0;
  char id[32];
  snprintf(id, sizeof(id), "%08lx-%08lx-%08lx", (unsigned long)esp_random(),
           (unsigned long)esp_random(), millis());
  eventId = id;
  int reading = LIGHT_SENSOR_PIN >= 0 ? analogRead(LIGHT_SENSOR_PIN) : -1;
  nightEvent = reading >= 0 && (DARK_WHEN_READING_LOW ? reading < DARK_THRESHOLD : reading > DARK_THRESHOLD);
  Serial.printf("Burst %s. Light reading: %d. Dark: %s\n", id, reading, nightEvent ? "yes" : "no");
  light(nightEvent);
  // Consume frames while automatic exposure/white balance settle under the
  // actual capture lighting. A delay alone can leave old frames in the queue.
  unsigned long settleStarted = millis();
  int warmupFrames = 0;
  while ((millis() - settleStarted < LIGHT_SETTLE_MS || warmupFrames < CAMERA_WARMUP_FRAMES)
         && millis() - settleStarted < LIGHT_SETTLE_MS + 1500) {
    camera_fb_t *old = esp_camera_fb_get();
    if (!old) { Serial.println("Camera warmup frame unavailable; attempting burst."); break; }
    esp_camera_fb_return(old);
    ++warmupFrames;
    delay(10);
  }
  Serial.printf("Camera settled: %d frames in %lu ms. White light: %s.\n",
                warmupFrames, millis() - settleStarted, nightEvent ? "on" : "off");
  sensor_t *sensor = esp_camera_sensor_get();
  if (!sensor) {
    light(false);
    Serial.println("Camera sensor unavailable at capture.");
    return;
  }
  const resolution_info_t &expected = resolution[sensor->status.framesize];
  if (sensor->id.PID == OV5640_PID || sensor->id.PID == OV3660_PID) {
    int gain = sensor->get_reg(sensor, 0x350A, 0x3FF);
    if (gain >= 0) Serial.printf("Sensor gain before burst: %.2fx\n", gain / 16.0f);
  }
  // Start the five-second recording immediately before the saved photo burst.
  startProjectAudioRecording();
  int captured = 0;
  const int maxAttempts = sensor->id.PID == OV3660_PID ? 4 : 2;
  for (int i = 0; i < BURST_COUNT; ++i) {
    unsigned long slotStarted = millis();
    // Recapture the same index while there is time left in this audio interval.
    for (int attempt = 1; attempt <= maxAttempts && !pending[i].data; ++attempt) {
      if (attempt > 1 && millis() - slotStarted >= EVENT_AUDIO_SECONDS * 1000UL / BURST_COUNT) break;
      camera_fb_t *fb = esp_camera_fb_get();
      const char *failure = nullptr;
      int decoderResult = -1;
      uint8_t *copy = nullptr;
      size_t bytes = fb ? fb->len : 0;
      if (!fb || !fb->buf) failure = "camera frame unavailable";
      else if (fb->width != expected.width || fb->height != expected.height)
        failure = "camera frame differs from the configured resolution";
      else if (fb->format == PIXFORMAT_RGB565 && sensor->id.PID == OV3660_PID)
        failure = encodeRgbFrame(fb, copy, bytes);
      else if (fb->format != PIXFORMAT_JPEG) failure = "unexpected camera pixel format";
      else if (bytes < 4 || bytes > MAX_PHOTO_BYTES) failure = "invalid JPEG byte count";
      else {
        copy = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!copy) failure = "insufficient PSRAM for the photo";
        else memcpy(copy, fb->buf, bytes);
      }
      if (fb) esp_camera_fb_return(fb);
      if (!failure) failure = checkJpeg(copy, bytes, expected, decoderResult);
      if (failure) {
        free(copy);
        Serial.printf("Photo %d/%d attempt %d/%d rejected: %s (%u bytes, decoder %d).\n",
                      i + 1, BURST_COUNT, attempt, maxAttempts, failure, unsigned(bytes), decoderResult);
      } else {
        pending[i].data = copy;
        pending[i].size = bytes;
        ++captured;
        Serial.printf("Photo %d/%d JPEG validated: %u x %u, %u bytes.\n",
                      i + 1, BURST_COUNT, unsigned(expected.width), unsigned(expected.height), unsigned(bytes));
      }
    }
    if (!pending[i].data)
      Serial.printf("Photo %d omitted after failed capture; event remains incomplete.\n", i + 1);
    // Keep the normal 500 ms spacing without adding decoder time to every gap.
    if (i + 1 < BURST_COUNT) {
      unsigned long elapsed = millis() - slotStarted;
      if (elapsed < PHOTO_INTERVAL_MS) delay(PHOTO_INTERVAL_MS - elapsed);
    }
  }
  light(false);
  if (!captured) discardProjectAudio();
  Serial.printf("Captured %d/%d photos; sending event to cloud dashboard.\n", captured, BURST_COUNT);
  if (captured < BURST_COUNT)
    Serial.println("Incomplete capture: missing evidence is not a normal AI result.");
  networkPreparationStarted = millis();
  uploadWindowStarted = false;
  beginProjectAiEvent(eventId, captured > 0 ? BURST_COUNT : 0);
  lastRetry = millis() - UPLOAD_RETRY_MS; // First attempt on next loop iteration.
}
}

void setupProjectSensors() {
  setupProjectAiAlarm();
  if (PIR_PIN >= 0) pinMode(PIR_PIN, INPUT);
  if (LIGHT_SENSOR_PIN >= 0) { pinMode(LIGHT_SENSOR_PIN, INPUT); analogReadResolution(12); }
  if (WHITE_LED_PIN >= 0) { digitalWrite(WHITE_LED_PIN, !WHITE_LED_ACTIVE_LEVEL); pinMode(WHITE_LED_PIN, OUTPUT); }
  Serial.println("PIR capture ready.");
}
void captureProjectWakeBurst() {
  captureBurst();
}
void pollProjectSensors() {
  if (hasPending() && uploadWindowStarted && !uploadTimeRemaining())
    discardPending("upload retry window expired; sleeping");
  if (hasPending() && !uploadWindowStarted && millis() - networkPreparationStarted >= UPLOAD_GIVE_UP_MS)
    discardPending("Wi-Fi/clock preparation timed out; sleeping");
  if (hasPending() && millis() - lastRetry >= UPLOAD_RETRY_MS) uploadPending();
  if (!hasPending()) {
    closePhotoUpload();
    bool talkBusy = pollProjectAudioTalk();
    bool aiBusy = pollProjectAiAlarm();
    if (!talkBusy && !aiBusy) sleepProjectUntilMotion();
  }
}
