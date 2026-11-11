# LCD On-Demand Preview Client

Firmware for the **Waveshare ESP32-S3-Touch-LCD-2.8 V2** with an ST7789 display and CST3530 touch controller. The project uses Arduino-ESP32 **3.3.12**, LVGL **8.3.10**, **16 MB Flash**, and **8 MB OPI PSRAM**. This directory contains the Arduino sketch. In the full project workspace, the companion camera firmware is located at `../camera`; it is not included in the LCD-only repository.

The Catppuccin UI provides a status home page and a full-screen monitor. Home shows configuration and Wi-Fi status with a Start action. Status shows connection, configuration, viewing, and backlight state. Setup explains USB provisioning. BOOT can start viewing from all three pages. The monitor retains the image at its native size and displays the countdown, frame sequence, frame age, Continue 60s, and Close controls. Paused or disconnected sessions clearly mark retained frames as stale. Closing a session or reaching its deadline returns to Home. Weather and clock features are not included.

## Directory Layout

- `lcd_viewer.ino`: initialization and main loop. Arduino compiles `src/` recursively.
- `src/board/`: display, touch, backlight, and JPEG rendering.
- `src/config/`: NVS configuration and USB provisioning.
- `src/preview/`: network preview client, MJPEG parser, session timing, and state.
- `src/ui/`: page views, UI controller, and fonts.
- `tests/`: host protocol and session tests, UI controller tests, and off-screen rendering fixtures.
- `tools/`: build manifests, off-screen rendering, and shared project paths.

`build.ps1` and `prepare_dependencies.py` are in this directory. Build outputs go to `build/lcd_viewer/` at the repository root. The dependency preparation script installs the shared LVGL library in `firmware/libraries/lvgl`.

The full local workspace also provides `tools/export_lcd_arduino.py` at the project root. The legacy `firmware/lcd_viewer/.build/arduino-data` directory and adjacent Arduino CLI configuration are shared with the camera build and should be retained there. These workspace tools and SDK caches are not part of the LCD-only repository.

## Build and Validation

Install Python, Arduino CLI, Arduino-ESP32 core **3.3.12**, and a host `g++` compiler. From this directory, run the following commands in PowerShell:

```powershell
python .\prepare_dependencies.py
.\build.ps1 -ArduinoCli 'path\to\arduino-cli.exe'
```

If `arduino-cli` is on `PATH`, run `.\build.ps1` without the `-ArduinoCli` argument.

`build.ps1` runs the LCD protocol and session tests with the host compiler before compiling the firmware. Use `-HostTestsOnly` to run only those tests. `-ConfigFile` accepts an existing Arduino CLI YAML configuration, and `-LibraryDirectory` accepts a directory containing the pinned LVGL library. The script builds the firmware without flashing a device.

All output paths below are relative to the repository root:

- Firmware images: `build/lcd_viewer/arduino/`
- Compilation log: `build/lcd_viewer/compile.log`
- Source and binary hashes: `build/lcd_viewer/build-manifest.json`
- Native UI screenshots and test logs: `build/lcd_viewer/ui-native/`

The build script's host tests cover MJPEG parsing and session behavior. For full view and controller validation, run `python tools/render_ui.py` from this directory. This requires MinGW GCC, G++, `ar`, Python, and Pillow. It links the production view with the pinned LVGL library and generates 16 off-screen PNGs in `build/lcd_viewer/ui-native/png/`, together with test logs. The colorful preview frames are synthetic test images, not live camera captures.

`lcd_viewer.ino.bin` is the application image. `lcd_viewer.ino.merged.bin` is a complete 16 MB factory image for flashing at address `0x0`; flashing it overwrites NVS configuration. Use the normal Arduino upload workflow for routine updates and retain NVS to preserve pairing. The build stages only the sketch entry point and `src/` files in `build/lcd_viewer/sketch/lcd_viewer/`, keeping SDK caches out of the sketch being compiled.

## Arduino IDE

For a clean checkout of the LCD-only repository, open `lcd_viewer.ino` in this directory and keep the entire adjacent `src/` directory. Run `prepare_dependencies.py` first. Set the IDE's Sketchbook location to the repository's `firmware` directory so it can find `firmware/libraries/lvgl`, then restart the IDE if the location changed.

In the full local workspace, use the separate copy at `../../arduino/lcd_viewer/lcd_viewer.ino`. After a successful build, run `python tools/export_lcd_arduino.py` from the project root to synchronize sources with the verified build manifest. The exporter refuses to overwrite local edits in the IDE copy.

Avoid opening a development sketch that contains the legacy `.build` SDK cache: the IDE may inspect compiler headers such as `extc++.h` and report **Invalid sketch filename**. Use the clean source checkout, staged sketch, or exported copy instead of renaming compiler headers.

Select **ESP32S3 Dev Module / Flash 16MB / OPI PSRAM / 16M Flash (3MB APP/9.9MB FATFS) / USB CDC On Boot Enabled**. Use the LVGL library prepared by the dependency script. The source rejects LVGL 9 and incompatible RGB565 configuration.

## Configuration and Operation

Provision the device through USB serial at **115200 baud** by sending one JSON object in the format shown in [setup.example.json](setup.example.json), followed by a newline. Supply the local Wi-Fi credentials, the camera's LAN address, and the same pairing token configured as `PROJECT_PREVIEW_TOKEN` on the camera. The viewer connects to the camera's LAN endpoint rather than the cloud upload endpoint. Configuration is stored in the display board's NVS, then the board restarts. Passwords and tokens are not echoed over serial. To update configuration later, stop viewing and send the complete JSON object again.

- Briefly press **BOOT (GPIO0)** during normal operation to start or stop viewing. Touch controls provide Start, Continue +60s, and Close.
- Each viewing session lasts 60 seconds. Continue starts a new 60-second countdown from the time it is pressed. The session lease is renewed every 10 seconds; video reception uses a separate task and connection and does not replace lease renewal.
- Stopping a session stops streaming and decoding. The backlight turns off after three seconds. Wi-Fi remains connected with modem sleep. Deep sleep and touch wake are not enabled.
- If no new frame has been decoded successfully for more than one second, the UI shows pause or reconnect status and the frame age. Pauses caused by PIR activity, enrollment, and identity processing have distinct status messages.
- BOOT still selects download mode when held during reset. During normal operation, use a short press for preview control. RESET and the battery power button are not preview buttons.
- GPIO7 keeps battery power enabled while the application runs. Long-press shutdown through the battery power button is not implemented; use BOOT or Close to stop viewing.

The default control port is **HTTP 80**, and the default MJPEG port is **81**. Both connections send `Authorization: Bearer ...`. Session IDs accept only URL-safe characters. The client does not follow arbitrary `stream_url` destinations: it requests `/stream?session_id=...` from the configured camera host and stream port. Communication uses plain HTTP and is intended for a trusted LAN.

## Memory and Concurrency

The client allocates two **128 KiB JPEG buffers**: one for the frame being parsed and one for the latest complete frame. While the UI decoder owns the second buffer, the receiver drops newly completed frames instead of queuing them. JPEG decoding uses two **320 x 240 RGB565 buffers**, so a failed decode does not damage the last successfully displayed image.

One UI task owns all LVGL operations, touch handling, decoding, and SPI display transfers. `lv_disp_flush_ready()` is called only after the synchronous SPI transfer completes.

The parser limits HTTP and part headers, chunk extensions, and JPEG sizes. It supports arbitrary fragmentation and chunked multipart streams, checks JPEG start/end markers, and passes frames to the decoder for validation. The client reconnects after connection loss or session loss. Generation checks prevent stale HTTP responses or old streams from affecting a new session. Repeated frame sequence numbers do not refresh the displayed frame timestamp.

## References

See [THIRD_PARTY.md](THIRD_PARTY.md) for pinned dependencies, licenses, and official driver sources. Successful compilation and host rendering do not replace validation of display output, touch, timing, and power behavior on the physical board. The documented UI update and source reorganization did not include device flashing.

The full local workspace also contains `docs/development/lcd-implementation.md` for integration and hardware acceptance checks, and `docs/development/lcd-ui-implementation.md` for the UI validation record and local firmware delivery. These documents are outside the LCD-only repository.
