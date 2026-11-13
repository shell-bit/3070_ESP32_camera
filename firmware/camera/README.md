# Camera firmware

The canonical camera source is `main/`, developed from [EE3070 commit cd6cbc9](https://github.com/benchan1228/EE3070/tree/cd6cbc9984225ae0afe9bf534cf3f664f015aae8/CameraWebServer_code-20261110T035708Z-1-001/CameraWebServer_code/CameraWebServer_Samplecode). The historical `.work/ee3070-hardware/` snapshot is no longer available. Current source fingerprints and new build logs must establish any verification claim; do not rely on that old path as a recoverable backup.

The default preview profile preserves PIR five-frame JPEG/audio upload and adds authenticated local MJPEG. The identity profile includes the real ESP-DL MSR/MNP detector and MFN embedding model, durable family enrollment, and V2 image/identity upload. The server controls exemptions; unknown identity never becomes a positive match.

Copy `main/project_local.example.h` to `main/project_local.h`, then configure Wi-Fi, assigned device credentials, server upload URL, and the same random pairing token as the LCD. No real credentials are included. Confirm the actual camera sensor, Flash capacity and PSRAM type before selecting the board profile or flashing.

Run from the repository root using the same installed SDK as Arduino IDE and the LCD build:

```powershell
$taskCli = (Resolve-Path 'E:/application/Arduino_IDE/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe').Path
$taskConfig = (Resolve-Path 'C:/Users/shell/.arduinoIDE/arduino-cli.yaml').Path
python firmware/camera/tools/run_host_tests.py
python firmware/camera/tools/build_arduino.py --cli $taskCli --flash-size 4 --config-file $taskConfig
python firmware/camera/tools/build_arduino.py --cli $taskCli --identity --flash-size 8 --config-file $taskConfig
```

The machine-local Arduino configuration above uses the installed SDK at `C:/Users/shell/AppData/Local/Arduino15` with Arduino-ESP32 3.3.12. It also keeps the Sketchbook at the repository's `firmware` directory, where shared libraries live under `firmware/libraries`. Any equivalent CLI installation and configuration with Arduino-ESP32 3.3.12 can be supplied; the legacy LCD `.build` SDK is not a build dependency. `--identity` requires at least 8 MB Flash and adds the pinned models under `vendor/esp-dl-models/`; `--flash-size 16` is also available. The build tool refuses a 4 MB identity profile. PSRAM is currently configured as octal; change `--fqbn` board options to match the actual device.

Artifacts and compile logs are written to `build-camera_preview/` or `build-camera_identity/`. Builds contain placeholder configuration until `project_local.h` is supplied. Flashing is a separate hardware step and has not been performed.

The dedicated face-store partition must be initialized on first use. Default mount behavior preserves an existing library by refusing automatic format. Only when intentionally provisioning that partition, temporarily set `PROJECT_FACE_ALLOW_FORMAT=1`, then restore `0`. A missing or corrupt/incompatible library cannot authorize family exemptions.

The Arduino build script stages the canonical `main/` files into generated build directories. CMake/IDF component metadata is also included, with separate build acceptance; it does not replace the documented Arduino verification route.

The generated `vendor/esp-dl-models/` directory and `build-manifest.json` are excluded from Git. On a fresh source checkout, the identity builder calls `tools/prepare_face_models.py` to retrieve wrappers, model files and their license from pinned Espressif commit `512ed0b0dc5f5320f3d9ca5a5aea189803f87f31`. Network access and the required Python dependencies are prerequisites. Verify that both `human_face_detect.espdl` and `human_face_feat.espdl` exist and are nonempty before accepting a build; manifest presence alone does not prove preparation completed. After a partial preparation failure, retry in a new isolated source checkout with an empty model directory, preserving existing evidence. Do not publish only the generated manifest or substitute personal face templates.

See the [repository README](../../README.md) for source boundaries and server/firmware contract checks. Historical implementation records are local workspace documentation and may not be present in a clone. Real hardware tests are still required for power use, 5 FPS / P95 latency, peak PSRAM, PIR preemption, controlled power loss, and recognition/actor-attribution accuracy.
