# LCD Viewer

The LCD viewer is a touchscreen terminal for the ESP32 monitoring system. It shows live video from the camera over the local network, displays connection status and time, and lets you set up Wi-Fi and camera pairing on the board. See the [system guide](../../README.md), [camera firmware](../camera/README.md), and [monitoring server](../../server/README.md).

## Hardware

This firmware targets the **Waveshare ESP32-S3-Touch-LCD-2.8 V2**. The V1 board has a different touch controller. [Waveshare board documentation](https://docs.waveshare.com/ESP32-S3-Touch-LCD-2.8)

| Hardware | Role in the system |
| --- | --- |
| ESP32-S3 with 2.4 GHz Wi-Fi | Connects to the camera over the LAN, receives and decodes JPEG frames, and synchronizes the display clock. |
| 2.8-inch ST7789 LCD | Shows the dashboard and native **320 × 240** live preview in landscape, using RGB565. |
| CST3530 capacitive touch | Supports on-screen configuration, navigation, and preview controls. |
| 8 MB OPI PSRAM | Holds compressed JPEG and decoded display buffers. The sketch requires PSRAM, including for Wi-Fi-only use. |
| 16 MB flash with NVS | Stores the firmware and retains the viewer’s Wi-Fi and camera settings across restarts. |
| BOOT button, GPIO0 | A short press wakes the backlight, starts or stops preview when paired, or goes back during Setup. Holding it during reset selects download mode. |
| PWM backlight, GPIO5 | Turns off during idle periods while the board remains on and connected to Wi-Fi. |
| Battery power hold, GPIO7 | Keeps battery power enabled while the application runs. |
| USB CDC serial | Provides a second way to save configuration at **115200 baud**. |

The viewer and the camera use separate boards and communicate over Wi-Fi. See [board support](src/board/board_port.cpp) for the panel and touch pin mapping.

## Build and upload

Use **Arduino-ESP32 3.3.12** and the pinned **LVGL 8.3.10** library. The PowerShell builder requires Arduino CLI, Python, and native `g++` on PATH. Run these commands from the repository root.

```powershell
python firmware/lcd_viewer/prepare_dependencies.py
& ./firmware/lcd_viewer/build.ps1
```

The dependency script prepares LVGL under `firmware/libraries/lvgl`. If Arduino CLI is outside PATH, pass `-ArduinoCli` with its executable path. If it uses a separate SDK configuration, pass `-ConfigFile` with the CLI YAML path. Firmware artifacts are written to `build/lcd_viewer/arduino/`, alongside the build log and manifest in `build/lcd_viewer/`.

For Arduino IDE, set the Sketchbook location to `firmware` under the repository root and open [lcd_viewer.ino](lcd_viewer.ino) with its adjacent `src/` directory. Use these **Tools** menu settings, then select the LCD board’s USB serial port and use **Upload**.

| Setting | Value |
| --- | --- |
| Board | ESP32S3 Dev Module |
| Flash Size | 16MB (128Mb) |
| Partition Scheme | 16M Flash (3MB APP/9.9MB FATFS) |
| PSRAM | OPI PSRAM |
| USB Mode | Hardware CDC and JTAG |
| USB CDC On Boot | Enabled |
| Erase All Flash Before Sketch Upload | Disabled for routine updates |

`lcd_viewer.ino.bin` is the application image. The merged factory image `lcd_viewer.ino.merged.bin` is written at flash address `0x0` and overwrites the viewer’s NVS settings. Use a normal Arduino upload with flash erase disabled to preserve pairing and Wi-Fi settings on the existing partition layout.

### Configure Wi-Fi and camera when uploading firmware

Create or edit `setup.json` next to `lcd_viewer.ino`. Replace the placeholders in this example with the LCD's Wi-Fi credentials and the camera's pairing settings:

```json
{
  "wifi_ssid": "YOUR_2_4_GHZ_WIFI",
  "wifi_password": "YOUR_WIFI_PASSWORD",
  "camera_host": "192.0.2.123",
  "token": "REPLACE_WITH_CAMERA_PREVIEW_TOKEN",
  "control_port": 80,
  "stream_port": 81
}
```

Use UTF-8 JSON without comments, at most 1024 bytes. All four string keys are required; the two integer ports are optional and default to 80/81 when omitted. Unknown or duplicate keys are rejected. `camera_host` accepts a LAN IPv4 address or hostname without a scheme, path or port. `token` must match the camera's 24–128 character `PROJECT_PREVIEW_TOKEN`. Set both camera strings to empty strings for Wi-Fi-only setup, or both Wi-Fi strings to empty strings for camera-only setup; at least one group must be complete. An empty password with a nonempty SSID means an open network.

Copy [build_opt.example.txt](build_opt.example.txt) to `build_opt.h` in the sketch directory and set its `LCD_SETUP_JSON_FILE` value to the absolute path of `setup.json`, using forward slashes. Update that path if you move the sketch. Existing camera-only build options must be replaced; `LCD_CAMERA_JSON_FILE` now causes a migration error. The old `camera_setup.json` file is no longer read and may be kept locally. Both JSON filenames and `build_opt.h` remain ignored by Git.

Save the files and click **Upload** in Arduino IDE with **Erase All Flash Before Sketch Upload** set to **Disabled**. The build embeds `setup.json` in the application and tracks it as a compiler dependency. Merely saving the file on the computer does not update the board. Without an embedding macro, the firmware continues to use saved settings, touchscreen Setup and USB provisioning.

At boot, new JSON contents replace the complete configuration, including Wi-Fi and ports. A device with no saved configuration can be provisioned this way directly. Successful application reports `Setup config applied from firmware` at 115200 baud. Unchanged contents preserve later touchscreen/USB edits on ordinary reboots; even whitespace changes count as new contents. Empty groups clear their previous values. Invalid JSON leaves the saved configuration unchanged. Existing unreadable records defer embedded setup so that previous-record recovery is preserved. If recording the applied revision fails, the next boot retries without repeatedly rotating the backup.

Keep local JSON files and firmware images private: they contain Wi-Fi credentials and the pairing token. The optional source exporter does not support provisioning resources, and the local release packager refuses a source set containing `setup.json`. Use the canonical sketch directly in Arduino IDE.

You can check provisioning without a camera: use your actual Wi-Fi settings, the example address above and a synthetic token of at least 24 characters. After upload, check **Setup → Camera** for the address, **Token** for the token-present indicator, and **Connection info** for the original/desired Wi-Fi SSID. Power-cycle and verify the settings remain. A preview connection failure is expected without a camera and does not indicate a configuration-write failure. The success log is emitted only when new contents are applied, so opening the serial monitor later may miss it.

## Connect and pair

1. Open **Setup → Wi-Fi**. Select a network from the scan list or enter its SSID manually, including hidden networks. Enter the password, or select **Use open network** where appropriate, then choose **Save & restart**. The network must support an ordinary Wi-Fi connection without a web sign-in page.
2. Open **Setup → Camera**. Enter the camera’s LAN hostname or IPv4 address and the same 24–128 character pairing token configured as `PROJECT_PREVIEW_TOKEN` on the camera. **Advanced** shows the control and stream ports, which default to **80** and **81**. Choose **Save & restart**.
3. With both groups saved, confirm the LCD can reach those ports on the camera and that the camera is running with deep sleep disabled. **Connection info** shows the LCD’s Wi-Fi link, IP address, signal strength, and clock status.

Either group can be saved first. Each touchscreen page saves only its own group and preserves the other. Wi-Fi alone enables connection status and the clock. **Back** or **BOOT** prompts before discarding an edited group, and Setup times out after 120 seconds without interaction. A Wi-Fi scan briefly interrupts the LCD’s connection before it reconnects using the saved credentials. Manual keyboard entry uses ASCII. Scan selection can preserve non-ASCII names.

### USB setup

Stop preview and open **Setup → USB setup**. In a serial terminal at **115200 baud**, send the JSON shape shown above as one complete line followed by a newline. A successful save writes the settings to NVS and restarts the board. If the firmware still contains new, unapplied setup contents, those contents can replace the USB settings on that restart; keep the two inputs consistent.

USB setup replaces the whole configuration. Include all four string keys. To save Wi-Fi only, set both `camera_host` and `token` to empty strings. To save only camera settings, set both `wifi_ssid` and `wifi_password` to empty strings. Emptying a group clears its previous values. Omitted port keys use 80 and 81.

## Using the viewer

- Tap **Start preview** or briefly press **BOOT** to open live video. Viewing lasts **60 seconds**. **Continue 60s** starts a fresh countdown from the moment you tap it. **Close** or another BOOT press stops viewing and returns to Home.
- PIR event capture, member enrollment, or identity processing can pause video. When the viewer has no fresh decoded frame for about **one second**, it keeps the last image on screen and indicates the pause. It shows the frame age and resumes as fresh images arrive while viewing is active.
- The clock shows **UTC+8** after SNTP synchronization. Until then it displays `--:--:--`. After syncing, it continues through a Wi-Fi outage until the next cold boot. Initial synchronization requires access to `pool.ntp.org` or `time.cloudflare.com`.
- With Wi-Fi configured, the backlight turns off **30 seconds** after startup, leaving preview or Setup, or pressing BOOT. Preview and Setup keep the screen lit. A short BOOT press also starts viewing if the camera is configured, and only wakes the screen if either group is missing. Wi-Fi stays connected when the backlight is off.

## LAN preview

The LCD sends authenticated session requests to the camera on HTTP port **80** and receives **320 × 240 MJPEG** directly from the camera on port **81**. Both connections use the pairing token in an HTTP Bearer header on a trusted LAN. Use the camera’s LAN address and pairing token for this connection. The monitoring server handles uploaded events and browser access separately.

The camera supports one active preview session with a **30-second lease**. The LCD renews it every **10 seconds** during viewing, independently of receiving video. Camera operations can pause frame production while control and lease renewal continue.

## Developer checks

The [Arduino sketch](lcd_viewer.ino) starts the board, configuration, networking, clock, and UI. Under `src/`, `board/` owns display, touch, and JPEG decoding, `config/` owns provisioning and NVS, `preview/` owns session control and MJPEG parsing, and `clock/` and `ui/` provide the visible interface. Two 128 KiB JPEG buffers and two 320 × 240 RGB565 buffers in PSRAM keep receiving and decoding separate without a growing frame queue.

The build runs the portable MJPEG and session checks first. To run just those checks from the repository root, use `& ./firmware/lcd_viewer/build.ps1 -HostTestsOnly`. For synthetic UI rendering and controller checks, use `python firmware/lcd_viewer/tools/render_ui.py` with the pinned LVGL library, GCC, G++, `ar`, and Python Pillow installed.

The focused embedded-setup host suite is separate: `python -B firmware/lcd_viewer/tools/run_config_tests.py`. It compiles the production configuration code with in-memory storage/serial test doubles and the existing `firmware/camera/vendor/cjson` parser. GCC and G++ plus that test-only sibling source are required; they are not extra LCD firmware dependencies. These checks do not establish physical NVS durability or camera connectivity.

Display, touch, Wi-Fi, preview timing, and power behavior require checks on the actual board. See [third-party dependencies and licenses](THIRD_PARTY.md).
