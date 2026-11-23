# Event audio and live two-way intercom

## What changed

On PIR wake, the ESP32-S3 records five seconds from the INMP441 while taking its
five camera photos. On the same connector, the OV3660 defaults to 640 × 480
RGB565 capture and the ESP32 encodes the JPEG. The OV5640 uses its native Full HD
JPEG output. Serial Monitor shows the detected sensor, resolution and encoder on
each capture. The camera uploads the photos first, then a mono 16 kHz, 16-bit WAV
file associated with the same device/event ID. Five seconds is 160,044 bytes
including the WAV header. Once Wi-Fi and verified HTTPS are ready, the photo
upload can start while the microphone finishes its clip. After the server
acknowledges the photo burst, the ESP32 sends a losslessly compressed WAV if the
server advertises support and the encoded clip saves at least 5%. The dashboard
stores the original audio samples. Serial Monitor reports both the number of
bytes sent and the elapsed upload time.
Before recording starts, the camera discards at least six warmup frames and
normally spends about 1.2 seconds adjusting exposure under the capture lighting.
The OV5640 noise profile keeps automatic exposure and white balance, limits gain
to 8x, applies mild denoising and reduces sharpening. Dark scenes may look darker;
illuminate the subject rather than increasing gain to hide poor lighting.
Set CAMERA_NOISE_REDUCTION to false to compare against the default sensor settings
after reflashing. This is a tuning change, not a verified repair of the physical camera.
The existing retry window remains bounded; failed uploads cannot keep it awake
forever. There is no SD-card storage in this implementation.

After uploads, the board checks the AI alarm and listens for dashboard speech.
A suspicious result or an armed classroom demo pulses the red LED and buzzer
together for 30 seconds, with half a second on and half a second off.
It stays available for 90 seconds while idle after uploads.
An active intercom call keeps it awake even when the dashboard microphone is muted.
Press End call to finish. After the call stops, the idle timer allows another
90 seconds before PIR sleep. The sketch now supports PIR + cloud operation only;
the local camera web page, streaming and always-on test mode were removed.
An HTTP request cannot wake it from deep sleep.

The live intercom streams audio in both directions through a persistent, verified
WebSocket connection. PCM is mono, 16 kHz, 16-bit, with 40 ms packets. Start intercom
to hear the INMP441, then Turn on microphone to speak through the MAX98357. Muting
your microphone keeps listening active. Live conversations are never written to
disk. Event recordings are separate from live calls and remain associated with
their photo burst. Audio does not go to YOLO or Qwen.

This is simultaneous two-way audio, with network delay. The ESP32 has no acoustic
echo canceller, so speaker sound can return through its microphone. Separate the
speaker and microphone, reduce speaker volume if necessary, and use headphones
at the dashboard. Real acoustic performance must be checked on the board.

## Wiring (same as the successful standalone test)

| Module pin | ESP32 / supply |
|---|---|
| INMP441 VDD | 3.3V |
| INMP441 GND and L/R | GND |
| INMP441 SCK | GPIO39 |
| INMP441 WS | GPIO40 |
| INMP441 SD | GPIO41 |
| MAX98357 VIN | Regulated 5V |
| MAX98357 GND | Common GND |
| MAX98357 BCLK | GPIO14 |
| MAX98357 LRC | GPIO38 |
| MAX98357 DIN | GPIO42 |
| MAX98357 SD/MODE | 3.3V for this wiring |
| MAX98357 GAIN | Unconnected |
| Speaker two wires | MAX98357 speaker + and - terminals |

The amplifier's SD pin is an enable/channel control; the microphone's SD pin is
data output. Never connect a speaker terminal to GND. The amplifier uses its 5V
supply, not a GPIO power output. All modules share ground. Keep the original PIR,
light sensor, white LED driver, buzzer and red LED driver connections unchanged.

With SD/MODE tied to 3.3V the amplifier remains powered during ESP32 sleep. I2S
clocks stop, but this is not full amplifier power-off. A future GPIO-controlled
SD pin or supply switch is required for lower total standby current.

## Settings you must check before uploading

1. Open CameraWebServer_Samplecode.ino from this folder.
2. Use **ESP32 by Espressif Systems 3.3.12**, ESP32S3 Dev Module, OPI PSRAM and
   16MB Flash. Select a partition with at least 3MB application space. Enable
   USB CDC On Boot if using the native USB port for Serial Monitor.
3. Copy wifi_settings.example.h to wifi_settings.h and set your actual 2.4GHz Wi-Fi
   name/password. The existing example uses the previous test network.
4. In project_settings.h keep LOCAL_DASHBOARD_UPLOAD_URL set to:
   `https://ee3070.panben218031.workers.dev/api/device/upload`
   After restarting the Cloudflare Quick Tunnel, update the Worker's `SERVER`
   address and deploy. The firmware URL stays the same.
5. The dashboard now uses shared access, with no user login or camera assignment.
   Keep `DEVICE_ID` and the existing `DEVICE_UPLOAD_TOKEN` in project_settings.h.
   The token identifies this registered camera for photo uploads, audio, talking
   and alarm polling. Removing browser login does not remove camera authentication.
6. Install **WebSockets by Markus Sattler 2.7.2** and **ArduinoJson 7.4.3** in
   Library Manager. These versions are already installed on this computer.
7. Upload the sketch. This has not been flashed automatically.

Never commit a populated token or private wifi_settings.h to a public repository.
Camera token rotation also changes authentication for photo uploads and AI alarm
polling. Keep the existing token unless the corresponding server registration changes.

Optional settings in project_settings.h:

```cpp
static const unsigned int EVENT_AUDIO_SECONDS = 5; // 3, 4 or 5
static const int PROJECT_MIC_GAIN = 4;
static const float PROJECT_SPEAKER_VOLUME = 1.0f;  // 100%; reduce if distorted
static const bool TALK_ENABLED = true;
static const unsigned long TALK_WINDOW_MS = 90000;
```

## Demonstration

1. Start the dashboard, AI worker and Cloudflare tunnel. Update the Worker's
   `SERVER` after each tunnel restart. Do not start ngrok.
2. Open the shared HTTPS dashboard; no sign-in is needed.
3. Power/reset the board. In low-power mode allow its initial 30-second PIR
   warmup, then move in front of the PIR after it arms.
4. Serial Monitor at 115200 should show simultaneous recording/burst capture,
   photo HTTP 201 responses and `Event audio upload HTTP 201`.
5. Open a suspicious or manual-review event. Its photos and recording appear
   together with a readable timestamp. Pending recordings stay hidden.
6. Wait for `INTERCOM READY` in Serial Monitor and **Ready** on the dashboard.
   Click **Start intercom** to hear the camera. Click **Turn on microphone** and
   allow permission to speak back. **Mute microphone** preserves listening.
7. Press **End call** when finished. Test with the speaker away from your ear.
   Receiving live audio confirms data delivery; actual sound still depends on
   microphone/speaker wiring, power and browser playback permission.

Use a browser such as Chrome on HTTPS (localhost also works for development).
There is no two-minute call limit. Brief interruptions reconnect automatically;
a lost browser connection expires after 30 seconds. End call or closing the page
releases your microphone. A disconnected microphone mutes uplink while preserving
listening. Keep the page visible: phones can suspend browser audio in the background.
Status distinguishes Ready, Busy, Sleeping, Offline, Audio hardware unavailable
and Firmware update needed. A sleeping device cannot be woken over the network.
The existing APK was not rebuilt or physically tested in this update.

## Server and privacy

- `intercom_api.py` adds the live relay; `audio_api.py` retains event recordings
  and the older HTTP talk API. `audio.js` and `audio-worklet.js` implement live
  controls and audio. `main.py` installs the relay. Install requirements-web.txt
  in the web environment; it now includes websockets. Run one dashboard process.
- The shared dashboard shows photos and audio together for suspicious/review events.
  Recordings stay hidden while an event is pending. Cleared/discarded event audio
  is deleted; retained audio has a maximum age of seven days. The running server
  performs cleanup. Stopping the server pauses this cleanup.
- The live relay authenticates the camera token and the browser session, validates
  Origin and CSRF, allows one call per camera, and uses bounded in-memory queues.
  Congestion drops late packets instead of building a long speech backlog.
  A browser disconnect pauses camera transmission; reconnection resumes the call.
  End call, lost-heartbeat expiry or changed device credentials revoke the call.
- HTTPS now uses the core's built-in CA bundle and hostname verification. It
  requires a valid clock: the sketch tries NTP for up to ten seconds. It does not
  fall back to setInsecure(). If uploads fail on a school network, verify that
  Wi-Fi has internet access and allows NTP, or try a phone hotspot.

## Validation and limitations

### Earlier OV3660 configuration — 17 November 2026

The camera GPIOs stay the same because the module was changed on the same
ESP32-S3 board and connector. The driver detects OV3660 automatically and prints
`Sensor: OV3660 (PID 0x3660)`. Full HD JPEG, five photos and five-second audio
remain enabled. The first captured JPEG's actual dimensions are printed too.

OV3660 uses automatic exposure/gain/white balance, vertical flip, brightness +1
and saturation -2, following Espressif's CameraWebServer example. The previous
OV5640 raw-register gain/noise profile only runs when an OV5640 is detected.
Both sensors receive their software-standby command before camera deinitialization.
Software standby is not a physical power switch; the module remains supplied.

`ESP32 local IP:` now prints immediately after Wi-Fi connects, even if clock
synchronization later fails. This change does not fix an NTP/network failure;
verified HTTPS still requires a valid clock. OPI PSRAM is still required on this
board. No server-side change is needed for the JPEG camera swap.

Driver reference: https://github.com/espressif/esp32-camera
Image settings: Arduino-ESP32 3.3.12 CameraWebServer example.
Verified compilation with Arduino-ESP32 3.3.12 and OPI PSRAM: 1,247,835 bytes
of application flash and 59,188 bytes of static RAM.
Actual OV3660 picture quality and wake/capture behavior require a hardware test.

### Live intercom update — 16 November 2026

The AutoDL web server has been updated and restarted. The existing Cloudflare
Worker successfully forwards the new WebSocket endpoints without a Worker change.
The YOLO/Qwen worker was left running. Backups are in
`/root/autodl-tmp/dashboard/intercom-backup-20261116-211925/` on the server and
`C:/Users/panbe/OneDrive/Desktop/EE3070/shared-dashboard/intercom/firmware-before-intercom/`
on this computer.

Verified:

- 10 relay checks: both directions, authentication/Origin/CSRF, call ownership,
  reconnecting, listening-only heartbeat, expiry, bounded queues and explicit stop.
- 31 shared-dashboard and event-audio regression checks.
- Chrome tests with a synthetic microphone: speaking, listening, mute, stop,
  temporary status-request failure and automatic WebSocket reconnection.
- Worklet capture/playback at 16 kHz, 44.1 kHz and 48 kHz browser sample rates.
- A 130-second synthetic call through the public Worker: 2,794 packets sent in
  each direction; 2,794 received by the camera simulator and 2,792 by the browser
  simulator. End call stopped transmission. These tests used no real microphone
  and created no event recordings. The temporary test camera was removed.
- ESP32-S3 build: 1,247,219 bytes of application flash and 59,188 bytes static RAM.

The new sketch has not been flashed automatically. Physical microphone input,
speaker loudness, echo and real-device latency still require a board test. The
dashboard reports Offline until the board connects; older firmware that reports
Ready shows Firmware update needed instead of offering an incompatible call.

### Diagnosing HTTP -1 uploads (10 November update)

HTTP -1 means the ESP32 failed to establish a connection; it is not an HTTP
response from the dashboard. The transport now prints the underlying TLS error,
Wi-Fi signal and available internal memory. It also prints the synchronized UTC
clock and allows 12 seconds for the TLS handshake and at least 10 seconds for the
photo TCP connection. Certificate verification stays enabled. The existing upload
retry window still applies, but a request already in progress can finish after it.

Upload the revised sketch and copy the `HTTPS clock`, `HTTPS failure`, `TLS error`
and memory lines from Serial Monitor if uploading still fails. These distinguish
clock/certificate, memory and connection failures. Wi-Fi connected means the board
joined an access point; it does not prove that network can reach the public server.

TLS error -29312 (EOF) means the TLS connection closed unexpectedly. It does not
identify who closed it or prove a bad certificate. A later compatibility update
explicitly offers HTTP/1.1 via ALPN and prints `HTTPS target: hostname -> address`.
The hostname stays in the TLS connection for SNI and certificate verification.
If EOF persists, compare the same firmware on home Wi-Fi and a phone hotspot;
send the clock, target and error lines. A successful hotspot test points toward
the home network/DNS path; failures on both require further TLS client diagnosis.

### Hardware check after cleanup

Compile with Arduino-ESP32 3.3.12, then upload and test the PIR burst, event audio,
AI alarm and dashboard speech on your board. Compiling does not flash the device.
The camera pin values, peripheral wiring, HTTPS verification and device token
remain unchanged. The old local web server and unsupported face-example code
were removed; this does not add cloud face enrolment support.

Backups were taken before editing. The new audio consumes additional power during
recording and the listening window. Upload failures and failed AI analysis remain
separate conditions; audio itself does not prove suspicious activity.
