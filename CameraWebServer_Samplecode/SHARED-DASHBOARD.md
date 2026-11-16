# Shared dashboard update — 16 November 2026

Open the dashboard directly. There is no user login and no need to assign this
camera to an account. Reports, event recordings and familiar-face management are
shared with anyone who can access the dashboard.

## Before flashing

- Keep the existing `DEVICE_ID` (`esp32-camera-1`) and `DEVICE_UPLOAD_TOKEN` in
  `project_settings.h`. The token is a camera credential, not a user password.
- Keep the fixed Worker upload URL:
  `https://ee3070.panben218031.workers.dev/api/device/upload`.
  When the Cloudflare Quick Tunnel restarts, update the Worker's `SERVER` address.
  The Arduino upload URL stays the same; ngrok is no longer used.
- Check the Wi-Fi name and password in `wifi_settings.h`, or the existing
  `wifi_settings.example.h` fallback if there is no private settings file.
- Open `CameraWebServer_Samplecode.ino`. Use Arduino-ESP32 3.3.12,
  ESP32S3 Dev Module, OPI PSRAM and 16 MB flash. The sketch includes its own
  `partitions.csv`. USB CDC On Boot should match the USB connection you use.
- No GPIO wiring changes are required. The firmware has not been flashed for you.

## Firmware changes

The sketch now supports PIR + cloud operation only. The local camera web page,
live streaming, serial burst command and always-on test mode were removed.
The current GPIO values and flash partition layout are unchanged.

All photo uploads, event audio, speaker requests and AI polling use the same
camera authentication helper. HTTP 401/403 photo failures stop retrying that burst
and explain which camera settings to check. Wi-Fi or server failures retain the
existing bounded retry behaviour.

The alarm handler accepts the server's `exempted` result and ends its wait without
sounding the alarm. Only the authenticated server may make that decision; enrolment
alone does not suppress alarms. `suspicious` still triggers the buzzer and red LED;
`normal` and `review` keep their previous behaviour. HTTPS certificate verification
remains enabled.

## Familiar-face recognition status

The server's family-member page and enrolment API are retained. The page automatically
selects a single camera; a selector appears only when several cameras are registered.
Each camera still requires its own saved face registrations.

This local Arduino sketch currently supports photo bursts, audio and AI alarms.
It does not poll the family enrolment command API, capture face-enrolment samples,
store face embeddings or send identity evidence with events. The old example's
onboard face functions were disabled under Arduino-ESP32 3.x and have now been
removed with the local web server. A compatible recognition implementation is still required
before Start enrolment can complete on this board. Do not treat a member profile
created on the dashboard as a successful face enrolment.

The server currently records identities without family alarm exemptions. This update
does not enable exemptions or change the YOLO/Qwen behaviour policy.

## Check after uploading

1. Serial Monitor at 115200 should show the camera ID and PIR warmup/wake messages.
2. Trigger the PIR. Confirm photo uploads return HTTP 200/201 and event audio uploads
   return HTTP 201.
3. Open the shared dashboard to view the event and its recording.
4. Start intercom while the camera is Ready. Listen and turn on your microphone to
   speak back. Muting keeps listening active; there is no two-minute call limit.
5. Press End call. The board returns to sleep after another 90 seconds of idle
   time. The dashboard shows Sleeping (last reported) after the camera's sleep message.
6. If the camera disconnects without a sleep message, the dashboard shows Offline.

Do not publish populated credentials or a compiled firmware image containing them.

Cleanup backup: the sibling folder
`CameraWebServer_Samplecode-backup-20261116-194319`.
Reopen the sketch before uploading so Arduino IDE loads the edited files from disk.
