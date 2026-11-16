# Camera firmware

The camera is the monitoring system's sensor and alarm unit. It records motion-triggered images and audio for the server, provides live video directly to the LCD viewer over Wi-Fi, and plays audio sent from the web dashboard. An optional firmware profile recognizes enrolled household members on the camera.

## Hardware and its role

These are the firmware's configured connections. The camera and the LCD viewer use separate ESP32 boards. Confirm the camera board revision, sensor, Flash capacity, PSRAM type, and wiring before choosing a build or flashing.

| Hardware | Role and firmware configuration |
| --- | --- |
| ESP32-S3 and camera sensor | The firmware uses the ESP32-S3-EYE pin map. A compatible sensor captures 1920 × 1080 JPEGs for events and 320 × 240 JPEGs for live preview. The driver uses sensor-specific standby only when it detects an OV5640. |
| PIR motion sensor | An active-high signal on GPIO 1 starts event capture after about 30 seconds of warm-up and an inactive reading. |
| Analog light sensor and white light | The reading on GPIO 2 determines whether to turn on the white light on GPIO 21 during capture. Calibrate the threshold for the sensor and installation. |
| INMP441 microphone | Records event audio at 16 kHz in mono. I2S0 uses GPIO 39 for BCLK, 40 for WS, and 41 for data in. The microphone runs at 3.3 V on the left slot. |
| MAX98357 amplifier and speaker | Play browser talkback audio. I2S1 uses GPIO 14 for BCLK, 38 for WS, and 42 for data out. |
| Red alarm light and buzzer | GPIO 48 and GPIO 47 drive the configured alarm outputs when the camera receives a `suspicious` result. Use suitable external drivers and power for the loads. |
| PSRAM and optional face storage | PSRAM holds camera frame buffers, copied JPEGs, and event audio. The identity profile uses a dedicated `face_store` Flash partition to retain local face templates across restarts. |

For the full sensor pin map, see [camera_pins.h](main/camera_pins.h). For peripheral pins and timing, see [project_settings.h](main/project_settings.h).

## How the camera operates

- **Event capture.** The camera attempts up to five FHD JPEGs and records about four seconds of audio. It sends photos in order, followed by the WAV recording, over HTTPS. The preview profile uses the V1 JPEG endpoint. The identity profile sends the original JPEG and identity metadata together using V2.
- **Live preview.** The LCD opens one authenticated session over HTTP on camera port 80 and receives QVGA MJPEG directly on port 81. The lease lasts 30 seconds and can be renewed. Event capture and local identity work pause preview. A still-valid session can resume while upload and server analysis continue.
- **Optional identity.** An assigned camera running the identity profile can receive enrollment and revocation commands from the server. It collects three consistent face samples at distinct poses, then confirms enrollment after saving the template locally. The server controls family exemptions, which are disabled by default.
- **Talkback.** Either build profile can play browser audio through the camera speaker.

## Configure the camera

Start the Web service described in the [server guide](../../server/README.md). Sign in as an administrator and open `/admin`. Under **Create an account**, create an account with **Access = User**, or reuse an existing user. Under **Register a camera**, enter the camera ID, select that user as **Owner**, and click **Register and generate token**. Copy the issued device token into the camera configuration.

If the camera is already assigned correctly and its token is available, reuse them. Saving an assignment again rotates the token immediately; the old firmware credential stops working. The administrator manages ownership; the assigned user manages family members.

Run the PowerShell examples in this guide from the repository root. Create the local configuration only if it is absent:

```powershell
if (-not (Test-Path 'firmware/camera/main/project_local.h')) {
    Copy-Item 'firmware/camera/main/project_local.example.h' 'firmware/camera/main/project_local.h'
}
```

Edit the Git-ignored `firmware/camera/main/project_local.h`.

| Setting | Value |
| --- | --- |
| `PROJECT_WIFI_SSID`, `PROJECT_WIFI_PASSWORD` | Credentials for a 2.4 GHz Wi-Fi network. For preview, the LCD and camera must be able to reach each other on the local network. |
| `PROJECT_DEVICE_ID`, `PROJECT_DEVICE_TOKEN` | The exact device ID registered on the server and the upload token issued when the device was assigned. The camera uses this token for server requests. |
| `PROJECT_UPLOAD_URL` | A device-reachable URL such as `https://your-server.example/api/device/upload`. Use this suffix for both profiles. The identity firmware derives the V2 URL from it. |
| `PROJECT_PREVIEW_TOKEN` | An independent random pairing token using 24–128 URL-safe ASCII characters. Enter the same token on the [LCD viewer](../lcd_viewer/README.md). Preserve an existing pairing token. An empty value disables preview HTTP; it does not disable enrollment. |
| `PROJECT_DEEP_SLEEP` | Keep this at `0` for enrollment and on-demand preview. Setting it to `1` enables PIR-wake operation, disables LAN preview, and skips enrollment-command polling. |
| `PROJECT_FACE_ALLOW_FORMAT` | Normally `0`. Set to `1` only for deliberate first-time initialization of an empty face store, as described below. |

For example, replace the placeholders below with your own values. Keep any existing valid credentials rather than replacing them with example text.

```cpp
#pragma once
#define PROJECT_WIFI_SSID "your-2.4GHz-network"
#define PROJECT_WIFI_PASSWORD "your-wifi-password"
#define PROJECT_DEVICE_ID "esp32-camera-1"
#define PROJECT_DEVICE_TOKEN "token-issued-by-your-server"
#define PROJECT_UPLOAD_URL "https://your-server.example/api/device/upload"
#define PROJECT_PREVIEW_TOKEN "replace-with-random-pairing-token"
#define PROJECT_DEEP_SLEEP 0
#define PROJECT_FACE_ALLOW_FORMAT 0
```

Keep the `/api/device/upload` suffix even for identity firmware: it derives the V2 upload, identity-state, and command URLs from this address. Server requests require working network time (SNTP) and an HTTPS certificate chain trusted by the firmware's built-in public CA bundle. A loopback address on the server is not reachable from the camera. Keep local credentials and personal media out of Git.

## Build and prepare the firmware

Use Python 3, Arduino CLI, and Arduino-ESP32 **3.3.12**. The build defaults to ESP32-S3 with octal (OPI) PSRAM. Select the Flash capacity that matches the board. The preview profile defaults to 4 MB Flash with a 3 MB application partition. The identity profile requires at least 8 MB Flash and defaults to a 6 MB application partition plus `face_store`. A 16 MB layout is also available. Windows build paths should avoid the `+` character.

Run this command from the repository root to build the 4 MB preview profile.

```powershell
python firmware/camera/tools/build_arduino.py --cli arduino-cli --flash-size 4
```

If Arduino CLI uses a separate SDK configuration, pass `--config-file <path-to-arduino-cli.yaml>`. Build outputs are in `firmware/camera/build-camera_preview/output/` or `firmware/camera/build-camera_identity/output/`. Follow the complete identity procedure below before attempting enrollment.

## Enable identity firmware and enroll a member

The identity profile uses ESP-DL face detection and feature models, with this project's enrollment and persistent storage code. The Web service and camera complete enrollment together; the Qwen GPU worker and LCD are not required. Enrollment does not automatically enable family alarm exemptions.

### 1. Choose the hardware profile and storage initialization mode

Confirm the camera's actual Flash capacity, OPI PSRAM compatibility, sensor, and pin map. Do not infer these from the separate LCD board. The identity builder rejects a 4 MB target. Its [8 MB layout](partitions-8mb-identity.csv) and [16 MB layout](partitions.csv) both contain a dedicated FAT partition named `face_store`, but at different offsets. These layouts have no OTA application slot.

On a previously initialized camera, leave `PROJECT_FACE_ALLOW_FORMAT` at `0` and preserve its partition layout. For first use on a device with no face templates to preserve, temporarily change the definition in `main/project_local.h` to:

```cpp
#define PROJECT_FACE_ALLOW_FORMAT 1
```

On startup, this permits formatting **only if mounting `face_store` fails**. It is not a routine repair setting: an unreadable existing store could be formatted and its templates lost. If the device already holds templates, preserve or back up its storage before any partition change or deliberate recovery. The server does not hold a backup of the camera's face templates.

A blank store normally needs one build and boot with formatting allowed, followed by another build and upload with the setting restored to `0`. Do not enroll members until the second firmware is running. Changing the header alone does not change the installed firmware.

### 2. Prepare the models and compile

Use your existing Python 3 and Arduino-ESP32 3.3.12 installation. No new Python environment is needed for these build scripts. On a fresh checkout, prepare the models:

```powershell
python firmware/camera/tools/prepare_face_models.py
if ($LASTEXITCODE -ne 0) { throw 'Face model preparation failed.' }

Get-Item 'firmware/camera/vendor/esp-dl-models/human_face_detect.espdl', `
         'firmware/camera/vendor/esp-dl-models/human_face_feat.espdl' |
    Select-Object Name, Length
```

Preparation downloads wrappers, weights, and their license from a pinned Espressif commit and packs the two model files. It needs network access for missing inputs. If a complete prepared model directory is already present, skip preparation. Both output files must exist and have nonzero lengths; `manifest.json` alone is not proof that packing finished.

Set the following variables in the same PowerShell session used for compilation and upload. Use `8` only for a confirmed compatible 8 MB target; use `16` for the 16 MB layout on a confirmed 16 MB target.

```powershell
$cameraRoot = (Resolve-Path 'firmware/camera').Path
$cameraCli = 'arduino-cli'
$cameraConfigArgs = @()
$cameraFlashMb = 8
$cameraFqbn = 'esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=huge_app,EraseFlash=none'
```

If the CLI is not on `PATH`, replace `$cameraCli` with the full path to the installed executable. If it needs the Arduino IDE's SDK configuration, set `$cameraConfigArgs` to `@('--config-file', 'C:/path/to/arduino-cli.yaml')`, using the actual existing file. The same configuration is passed to both commands below.

```powershell
python "$cameraRoot/tools/build_arduino.py" `
    --cli "$cameraCli" @cameraConfigArgs `
    --fqbn "$cameraFqbn" `
    --identity --flash-size $cameraFlashMb
if ($LASTEXITCODE -ne 0) { throw 'Identity build failed; do not upload old outputs.' }
```

`--identity` supplies `PROJECT_FACE_ENABLED=1`, `PROJECT_UPLOAD_V2=1`, the model flags, and embedded model data. Do not override these flags in `project_local.h`. Despite the `PartitionScheme=huge_app` board option, the builder supplies the project's custom `partitions.csv` and application size for the selected capacity. Set capacity through `--flash-size`, not by adding `FlashSize` to the builder's `--fqbn` argument.

Continue only after a successful compile. The log is `firmware/camera/build-camera_identity/compile.log`; firmware files are in `firmware/camera/build-camera_identity/output/`.

### 3. Upload and finish first-time storage initialization

Connect the camera by USB and identify its port. Distinguish it from the LCD board:

```powershell
& $cameraCli @cameraConfigArgs board list
```

Close any serial monitor using that port. Replace `COMx` with the camera's actual port, then upload the successfully compiled identity output:

```powershell
$cameraPort = 'COMx'
& $cameraCli @cameraConfigArgs upload `
    --port "$cameraPort" `
    --fqbn "${cameraFqbn},FlashSize=${cameraFlashMb}M" `
    --input-dir "$cameraRoot/build-camera_identity/output" `
    "$cameraRoot/build-camera_identity/camera_identity"
if ($LASTEXITCODE -ne 0) { throw 'Identity upload failed.' }
```

This uploads the separate bootloader, partition, and application files using the installed Arduino core's upload recipe. It does not compile the sketch. The first switch from the preview layout must include the identity partition table; an application-only upload cannot create a missing `face_store` partition. This is a partition-layout change, so preserve any existing device data before proceeding.

Keep `EraseFlash=none`. Do not substitute a merged factory image or a full-chip erase when preserving initialized storage. With the same identity layout, the ordinary upload above does not write the `face_store` region; changing between the 8 MB and 16 MB layouts changes that region.

This is not a guarantee that all other Flash data survives. Arduino-ESP32 3.3.12 also writes `boot_app0.bin` at `0xe000`; with the current camera partition tables, that write overlaps the end of NVS and the PHY partition. Preserve any needed data in those regions before using this upload procedure. Camera Wi-Fi and credentials in this project are compiled from `project_local.h`.

If this was a first-time build with formatting allowed:

1. Let the camera boot and attempt to mount or initialize the store. Serial logging uses 115200 baud. If the following message appears, initialization failed; check the selected capacity, uploaded partition table, and format setting before continuing:

   ```text
   Face store unavailable; initialize dedicated FATFS partition before enrollment.
   ```

2. Restore `#define PROJECT_FACE_ALLOW_FORMAT 0` in `main/project_local.h`.
3. Repeat the compile and upload commands with the same Flash capacity, partition layout, and erase-disabled option. Then let the camera boot again before enrollment.

There is no dedicated store-initialization success log. `Camera online` only indicates that startup reached that message; it does not prove Wi-Fi connectivity, model readiness, or successful registration. Successful enrollment with formatting disabled, followed by a restart and recognition check, provides stronger verification.

### 4. Enroll through the Web interface

Sign in as the camera's assigned **User**, not the administrator, and open `/members` on the server. The current member page uses Chinese labels:

1. Select the camera under **Current camera** (`当前相机`).
2. Enter a **Display name** (`显示名称`) and click **Add member** (`添加成员`). Creating a member record alone does not store a face template.
3. Keep the camera powered and connected. Wait for any PIR event capture, upload, and result handling to finish, then click **Start enrollment** (`开始登记`).
4. When the page shows **Sampling** (`正在采样`), keep exactly one person in view, with a clearly lit, sharp face. Try facing forward, then turning slightly left and right, pausing about 1–2 seconds at each pose. The camera needs three accepted samples of the same person at sufficiently different poses; holding one pose will not complete enrollment.
5. Wait for **Registered** (`已登记`). If it fails or expires, resolve the displayed cause and start a new enrollment.

The command expires **120 seconds after the server creates it**, even if the camera is offline or starts sampling late. The camera normally polls commands about every three seconds; no PIR trigger or photo upload is needed to receive a command. Only one enrollment can be pending or sampling on a camera at a time.

PIR events take priority and clear the samples already collected for enrollment without restarting the deadline. Sit or stand still and turn your head gently; repeated motion events can prevent completion. Enrollment temporarily uses VGA capture and pauses LCD preview.

The camera saves the templates locally, reads the saved record back, and sends a `committed` acknowledgement with member, recognizer, and database versions. Only a valid acknowledgement makes the page show **Registered**. A recent device heartbeat or **Sampling** status is not a successful enrollment. See the [camera command handler](main/project_device_commands.cpp) and [member interface](../../server/members.js).

### 5. Check persistence and normal operation

Restart the camera with `PROJECT_FACE_ALLOW_FORMAT=0`, allow it to reconnect, and refresh `/members`. A page retaining an old registration status alone does not prove that the device reloaded and matched its templates; the default member UI confirms registration, not individual event matches.

For an additional recognition check, trigger a controlled event with the enrolled person after restart. A server operator can inspect that new event's stored V2 metadata read-only in `frame_identities.metadata`: a `faces` entry should contain `status: "matched"` and the expected `member_id`. Match the event/device and use a newly captured frame, not an older record. See [identity ingestion and storage](../../server/identity_ingest.py). A local match is still separate from server authorization and an alarm-exemption decision.

Each camera keeps its own templates, so repeat enrollment for every camera that should recognize the member. Reassigning a camera or rotating its device credential invalidates the old registration. Disabling or deleting a member revokes server authorization; the camera receives the local revocation command when connected. Re-enabling a member requires enrollment again.

Family alarm exemptions remain disabled by default. Successful registration does not automatically suppress alarms. Real-device tests are still needed for recognition accuracy, lighting and occlusion, PIR interruptions, and storage persistence; compiling firmware or receiving an acknowledgement does not establish those results.

## Identity and enrollment troubleshooting

| Symptom | What to check |
| --- | --- |
| Identity build rejects 4 MB | Confirm the camera's physical Flash. This profile requires at least 8 MB; selecting a larger value cannot increase the chip's capacity. |
| Model file missing or empty, even though `manifest.json` exists | Rerun `prepare_face_models.py`, resolve its download or packing error, and verify both packed `.espdl` files before rebuilding. |
| `Face recognition disabled in this build` in serial output | Build with `--identity` and upload the identity output, not the preview output. Remove conflicting face/V2 flag definitions from local configuration. |
| `Face store unavailable` or `Recognition model or dedicated face storage unavailable` | Check the identity partition table, actual Flash size, model build, and first-time initialization procedure. Do not enable formatting on a store containing templates you need to preserve. |
| No camera is available on `/members`, or the page rejects the account | Use the assigned ordinary user account and have the administrator verify ownership. The administrator account is for account/device management. |
| Waiting for camera connection (`等待相机上线`) | Check 2.4 GHz Wi-Fi, the exact device ID and current device token, `PROJECT_DEEP_SLEEP=0`, HTTPS certificate trust, SNTP, and the `/api/device/upload` URL suffix. An HTTP URL or a direct `/upload-v2` suffix will not produce valid command URLs. |
| `Clock not synchronized yet` | Allow the camera to reach an SNTP server. Verified HTTPS waits for valid time; do not bypass certificate verification. |
| Sampling never completes or enrollment expires | Use one clearly visible face and three distinct poses of the same person. Wait for ongoing events to finish and reduce PIR interruptions. The 120-second deadline starts at task creation; retry after the failed task clears. |
| `This camera already has an enrollment in progress` | Finish the existing task or wait for it to expire before starting another member on that camera. |
| `Feature database commit failed or capacity reached` | Inspect serial errors and available template capacity/storage. The page must not be treated as registered until saving and acknowledgement succeed; do not erase the store as a routine retry. |
| Registration is revoked after a device assignment or token change | Update the firmware with the new device token, rebuild/upload with formatting disabled and the same layout, and enroll again under the current owner. |
| LCD preview pauses while enrolling | Expected: enrollment owns the camera temporarily. A still-valid preview lease can resume afterward. |
| No application logs on the connected USB port | The default board configuration disables USB CDC on boot. Check the board's UART/native-USB connection; for native USB logging, use the appropriate `CDCOnBoot=cdc` board option consistently when rebuilding and uploading. |
| Alarms continue after registration | Expected with the default identity-recording mode. Family alarm exemption is a separate server policy, not an enrollment switch. |

## Delivery and alarm limits

- Unsent event media is held in RAM for up to about 60 seconds of upload attempts and is lost on reset. The server's separate collection window defaults to 30 seconds from the first accepted frame. Incomplete events enter manual review when that window expires.
- After the upload phase, the camera polls the event result every three seconds for up to 60 seconds. It alarms when it receives `suspicious` during that window. A `review` response ends polling, so a later manual decision may update the dashboard without triggering the camera.
- The camera handles one event at a time. Motion requests may expire while uploads or result polling are in progress.

See the [project README](../../README.md) for the system architecture and startup order.
