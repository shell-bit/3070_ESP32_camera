# LCD On-Demand Preview Client

Firmware for the **Waveshare ESP32-S3-Touch-LCD-2.8 V2** with an ST7789 display and CST3530 touch controller. The project uses Arduino-ESP32 **3.3.12**, LVGL **8.3.10**, **16 MB Flash**, and **8 MB OPI PSRAM**. This directory contains the Arduino sketch. In the full project workspace, the companion camera firmware is located at `../camera`; it is not included in the LCD-only repository.

The Catppuccin UI provides Home, Status, on-screen Setup, and a full-screen monitor. Setup independently saves the LCD's Wi-Fi and camera endpoint using a touch keyboard, with USB provisioning retained as a fallback. Wi-Fi and the network-synchronized clock work without camera settings. The monitor retains the image at its native size and displays the countdown, frame sequence, frame age, Continue 60s, and Close controls. Paused or disconnected sessions clearly mark retained frames as stale. Closing a session or reaching its deadline returns to Home.

## Directory Layout

- `lcd_viewer.ino`: initialization and main loop. Arduino compiles `src/` recursively.
- `src/board/`: display, touch, backlight, and JPEG rendering.
- `src/config/`: checked NVS persistence, USB provisioning, and the shared Setup draft.
- `src/clock/`: nonblocking SNTP service and UTC+8 clock formatting.
- `src/preview/`: network preview client, MJPEG parser, session timing, and state.
- `src/ui/`: page views, UI controller, and fonts.
- `tests/`: host protocol and session tests, UI controller tests, and off-screen rendering fixtures.
- `tools/`: build manifests, off-screen rendering, and shared project paths.

`build.ps1` and `prepare_dependencies.py` are in this directory. Build outputs go to `build/lcd_viewer/` at the repository root. The dependency preparation script installs the shared LVGL library in `firmware/libraries/lvgl`.

The full local workspace shares the Arduino IDE installation's ESP32 SDK at `C:/Users/shell/AppData/Local/Arduino15`, using `C:/Users/shell/.arduinoIDE/arduino-cli.yaml`. Keep SDK data outside this sketch. The root `tools/export_lcd_arduino.py` remains an optional distribution utility; routine editing and IDE use open this canonical source directory directly. These workspace tools and the installed SDK are not part of the LCD-only repository.

## Build and Validation

Install Python, Arduino CLI, Arduino-ESP32 core **3.3.12**, and host `gcc`/`g++` compilers. From this directory, run the following commands in PowerShell:

```powershell
python .\prepare_dependencies.py
.\build.ps1 -ArduinoCli 'path\to\arduino-cli.exe'
```

If `arduino-cli` is on `PATH`, run `.\build.ps1` without the `-ArduinoCli` argument.

For the existing full local workspace, run these commands from the repository root to share the IDE's installed Arduino-ESP32 **3.3.12** and the pinned library at `firmware/libraries/lvgl`:

```powershell
$taskCli = (Resolve-Path 'E:/application/Arduino_IDE/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe').Path
$taskConfig = (Resolve-Path 'C:/Users/shell/.arduinoIDE/arduino-cli.yaml').Path
& $taskCli core list --config-file $taskConfig
& ./firmware/lcd_viewer/build.ps1 -ArduinoCli $taskCli -ConfigFile $taskConfig
```

These installed paths are machine-local; a different checkout must supply its own CLI and configuration with the same pinned core.

`build.ps1` runs the retained MJPEG/session host suite through `tools/run_host_tests.py` before compiling the firmware. Use `-HostTestsOnly` to run that suite without compiling firmware. `-ConfigFile` accepts an existing Arduino CLI YAML configuration, and `-LibraryDirectory` accepts a directory containing the pinned LVGL library. The restored script builds the firmware and records matching source/artifact evidence without flashing a device.

All output paths below are relative to the repository root:

- Firmware images: `build/lcd_viewer/arduino/`
- Compilation log: `build/lcd_viewer/compile.log`
- Source and binary hashes: `build/lcd_viewer/build-manifest.json`
- Native UI screenshots and test logs: `build/lcd_viewer/ui-native/`

For the retained preview view/controller checks, run `python tools/render_ui.py` from this directory. This requires MinGW GCC, G++, `ar`, Python, and Pillow. It links the production views and Setup controller with the pinned LVGL library and generates the 16 scenes listed in `tests/ui_fixtures.json` in `build/lcd_viewer/ui-native/png/`, together with test logs and source hashes. The colorful preview frames are synthetic test images, not live camera captures. The configuration test cJSON copy was removed; firmware continues using the SDK's cJSON.

The user-approved test cleanup removed dedicated configuration persistence, Setup editing/saves, scan lifecycle, clock and Wi-Fi-only integration tests, plus their feature-specific UI scenes and memory cycles. The corresponding firmware features remain implemented. The retained suite covers protocol/session behavior, preview controls, generation handling, basic page rendering and existing backlight timing; it does not establish the removed coverage.

`lcd_viewer.ino.bin` is the application image. `lcd_viewer.ino.merged.bin` is a complete 16 MB factory image for flashing at address `0x0`; flashing it overwrites NVS configuration. Use the normal Arduino upload workflow for routine updates and retain NVS to preserve pairing. The build stages only the sketch entry point and `src/` files in `build/lcd_viewer/sketch/lcd_viewer/`, keeping SDK caches out of the sketch being compiled.

## Arduino IDE

Open `firmware/lcd_viewer/lcd_viewer.ino` from the repository root, keeping the entire adjacent `src/` directory. This is the single editable LCD source tree in both the full workspace and an LCD-only checkout. If the pinned library is missing, run `prepare_dependencies.py` first. Set the IDE's Sketchbook location to the repository's `firmware` directory so it can find `firmware/libraries/lvgl`, then restart the IDE if the location changed.

The current local Sketchbook location is `E:/Code/project/3070_ESP32_camera/firmware`. IDE and CLI use the installed SDK through `C:/Users/shell/.arduinoIDE/arduino-cli.yaml`; an `arduino/lcd_viewer` export is not needed for development. Generated staging under `build/lcd_viewer/` is build output and should not be edited.

Keep SDK and compiler caches outside the sketch. A nested SDK can make the IDE inspect compiler headers such as `extc++.h` and report **Invalid sketch filename**; do not rename SDK headers to work around that layout problem.

Select **ESP32S3 Dev Module / Flash 16MB / OPI PSRAM / 16M Flash (3MB APP/9.9MB FATFS) / USB CDC On Boot Enabled**. Use the LVGL library prepared by the dependency script. The source rejects LVGL 9 and incompatible RGB565 configuration.

## Configuration and Operation

In **Setup**, choose **Wi-Fi** to scan nearby networks or manually enter a network name (SSID), including a hidden network. Enter the LCD's Wi-Fi password using the on-screen keyboard, or explicitly select **Use open network** to clear a saved password. Press **Save & restart** on the Wi-Fi page. The LCD connects and can synchronize its clock without a camera address or Token. Open networks requiring a web login are not supported.

When ready to use video, connect the LCD and camera to the same reachable Wi-Fi/LAN. Use **Camera** to set the camera's hostname or IPv4 address and matching pairing token, and **Advanced** for the control/stream ports (defaults 80/81). Press **Save & restart** on the Camera page. Camera Wi-Fi is configured separately in its firmware. Camera settings can also be saved before Wi-Fi, but preview requires both groups. Tokens remain required for camera access; touch Camera Save requires at least 24 characters. Secret fields provide Keep/Replace controls and are masked immediately.

Each section edits its own group. Wi-Fi Save preserves the saved camera fields, including an unchanged legacy token; Camera Save preserves saved Wi-Fi. The Setup menu has no global Save action. Back from a changed section asks whether to discard its edits before returning to the menu; Keep editing returns to that section. Cancel does not write. Save verifies the previous-record backup and the new NVS record before restarting. On failure, the draft remains available for correction/retry. Boot can recover an invalid primary from the previous verified record. Configuration is applied only after restart. BOOT follows Back throughout Setup, and 120 seconds without interaction discards edits and returns Home.

Configuration keeps the original 360-byte NVS layout. A record can contain Wi-Fi only, camera only, or both complete groups; a half-filled group is rejected. A valid partial primary takes precedence over an older complete backup. Older firmware that required both groups cannot use a Wi-Fi-only record and may restore old backup credentials or appear unconfigured. Before any separately authorized downgrade, save and verify a complete primary compatible with that firmware; the previous-record backup is not a permanent downgrade copy.

Scanning is asynchronous and shows at most 16 strongest distinct network/security pairs. It temporarily disconnects LCD Wi-Fi, then reconnects with the saved boot configuration. Start remains blocked while scan cleanup is pending. Keyboard entry is ASCII; scanned non-ASCII names retain their original bytes and use escaped display labels with an access-point suffix. There is no Chinese input method in this version.

**Connection info** shows the LCD's live Wi-Fi/IP/signal and clock date/status. Home and Monitor show `HH:MM:SS` in **Beijing time (UTC+8)**. SNTP starts after Wi-Fi connects and requires access to a time server (`pool.ntp.org` or `time.cloudflare.com`). Until the first successful synchronization, the display reads `--:--:--`; after synchronization, it continues locally through Wi-Fi loss. A cold boot without synchronization has no valid time. Viewing/session deadlines still use monotonic milliseconds.

Home distinguishes actual Wi-Fi connectivity from camera configuration. With Wi-Fi alone, the clock operates and Start preview stays disabled. The backlight turns off 30 seconds after UI startup, leaving Setup, stopping preview or pressing BOOT. Switching Home/Status tabs does not restart this timer; this is not a general touch-inactivity timeout. In Wi-Fi-only mode, BOOT wakes the screen without starting a camera request. Setup remains illuminated until its separate 120-second inactivity timeout. Without saved Wi-Fi, the display stays on for setup.

Provision the device through USB serial at **115200 baud** by sending one JSON object in the format shown in [setup.example.json](setup.example.json), followed by a newline. Supply the local Wi-Fi credentials, the camera's LAN address, and the same pairing token configured as `PROJECT_PREVIEW_TOKEN` on the camera. The viewer connects to the camera's LAN endpoint rather than the cloud upload endpoint. Configuration is stored in the display board's NVS, then the board restarts. Passwords and tokens are not echoed over serial. To update configuration later, stop viewing and send the complete JSON object again.

USB JSON replaces the **whole configuration**; unlike a touchscreen section save, it does not merge omitted groups. All four string keys remain required. To provision Wi-Fi only, explicitly set both `camera_host` and `token` to empty strings; this clears any saved camera group. Camera-only provisioning similarly requires empty `wifi_ssid` and `wifi_password`. Missing keys, half-filled groups and an all-empty configuration are rejected. Omitted control/stream ports retain their existing default behavior (80/81). The legacy USB token rule is unchanged.

Serial provisioning is disabled while a touch draft or scan cleanup owns configuration. **USB setup** first leaves/discards the draft and drains incomplete serial input before enabling provisioning. Send a fresh complete JSON line after entering that page.

- Briefly press **BOOT (GPIO0)** outside Setup to start or stop viewing when both groups are configured. With incomplete preview configuration it only wakes the screen. In Setup it navigates back or opens the discard confirmation. Touch controls provide Start, Continue +60s, and Close.
- Each viewing session lasts 60 seconds. Continue starts a new 60-second countdown from the time it is pressed. The session lease is renewed every 10 seconds; video reception uses a separate task and connection and does not replace lease renewal.
- Stopping a session stops streaming and decoding. The backlight turns off after 30 seconds. Wi-Fi remains connected with modem sleep. Deep sleep and general touch wake are not enabled.
- If no new frame has been decoded successfully for more than one second, the UI shows pause or reconnect status and the frame age. Pauses caused by PIR activity, enrollment, and identity processing have distinct status messages.
- BOOT still selects download mode when held during reset. During normal operation, use a short press for preview control. RESET and the battery power button are not preview buttons.
- GPIO7 keeps battery power enabled while the application runs. Long-press shutdown through the battery power button is not implemented; use BOOT or Close to stop viewing.

The default control port is **HTTP 80**, and the default MJPEG port is **81**. Both connections send `Authorization: Bearer ...`. Session IDs accept only URL-safe characters. The client does not follow arbitrary `stream_url` destinations: it requests `/stream?session_id=...` from the configured camera host and stream port. Communication uses plain HTTP and is intended for a trusted LAN.

## Memory and Concurrency

The client allocates two **128 KiB JPEG buffers**: one for the frame being parsed and one for the latest complete frame. While the UI decoder owns the second buffer, the receiver drops newly completed frames instead of queuing them. JPEG decoding uses two **320 x 240 RGB565 buffers**, so a failed decode does not damage the last successfully displayed image.

The existing buffers and PSRAM checks remain allocated at boot even in Wi-Fi-only mode. The control worker always exists for networking/scans; the stream worker is created only when both configuration groups are ready. No camera HTTP/MJPEG request is made while camera configuration is absent.

One UI task owns all LVGL operations, touch handling, decoding, and SPI display transfers. `lv_disp_flush_ready()` is called only after the synchronous SPI transfer completes.

The parser limits HTTP and part headers, chunk extensions, and JPEG sizes. It supports arbitrary fragmentation and chunked multipart streams, checks JPEG start/end markers, and passes frames to the decoder for validation. The client reconnects after connection loss or session loss. Generation checks prevent stale HTTP responses or old streams from affecting a new session. Repeated frame sequence numbers do not refresh the displayed frame timestamp.

## References

See [THIRD_PARTY.md](THIRD_PARTY.md) for pinned dependencies, licenses, and official driver sources. Successful compilation and host rendering do not replace validation of display output, touch, timing, and power behavior on the physical board. The documented UI update and source reorganization did not include device flashing.

The full local workspace also contains `docs/development/lcd-implementation.md` for integration and hardware acceptance checks, and `docs/development/lcd-ui-implementation.md` for the UI validation record and local firmware delivery. These documents are outside the LCD-only repository.
