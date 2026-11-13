# 3070_ESP32_camera

ESP32 monitoring system with PIR-triggered image/audio capture, camera-side
identity processing, an LCD viewer, and a Python web/AI backend. Source and
software verification do not establish physical hardware acceptance.

| Component | Canonical source and entry point |
| --- | --- |
| Camera | [firmware/camera](firmware/camera/README.md); `main/main.cpp` |
| LCD viewer | [firmware/lcd_viewer](firmware/lcd_viewer/README.md); `lcd_viewer.ino` |
| Server | `server/main.py` (web API), `server/worker.py` (worker) |
| Firmware/server contract | `tests/test_firmware_contract.py` |

## Source relationship

The camera was developed from
[EE3070 commit cd6cbc9](https://github.com/benchan1228/EE3070/tree/cd6cbc9984225ae0afe9bf534cf3f664f015aae8/CameraWebServer_code-20261110T035708Z-1-001/CameraWebServer_code).
The local implementation retains event capture/audio and adds managed camera
ownership, authenticated LCD preview sessions, family enrollment and V2 uploads.
It is the canonical implementation; the older upstream sketch is not a second
editable camera tree.

Historical project records identify the server baseline as a dashboard source
export from 2026-11-11, followed by local integration. EE3070 contains an alarm
API extension, not this complete server. This repository does not claim a shared
Git history with EE3070 or a repository-wide license for upstream code. Preserve
existing third-party copyright and license notices.

## Local configuration and dependencies

- Use Arduino-ESP32 **3.3.12** and the component-specific build instructions.
  LCD LVGL dependencies are pinned by its existing lock file.
- Camera configuration is copied from
  `firmware/camera/main/project_local.example.h` to the ignored
  `project_local.h`. Configure Wi-Fi, the assigned device token/server URL and
  the LCD pairing token locally. The checked-in defaults are placeholders.
- LCD provisioning uses its own documented setup flow and example. Camera and
  LCD board capacities and GPIOs must be confirmed independently.
- Server paths and credentials are supplied through environment/configuration,
  including `CAMERA_DATA_DIR` and notification credentials. Use a separate
  synthetic data directory for local validation, never a production database.
- GPU inference needs the existing working Qwen/Torch/model environment.
  `server/requirements-worker.txt` supplements that environment; it does not
  recreate it. CPU regression success is not GPU/model acceptance.

The source allowlist intentionally excludes credentials, personal/test media,
databases, generated firmware, model caches, SDKs and tool binaries. Update the
allowlist deliberately when adding source files; do not force-add private data.
Local agent documents, historical deployment records and unrelated research
tools are not part of this source distribution.

## Software checks

For Windows PowerShell, create an isolated Python environment and install the
Web/CPU regression dependencies. These commands do not start a server or worker:

```powershell
python -m venv .venv
& ./.venv/Scripts/python.exe -m pip install -r server/requirements-web.txt -r server/requirements-test.txt
& ./.venv/Scripts/python.exe -B server/run_tests.py --log-dir build/server-tests/local
node --test server/test/members.test.js
& ./.venv/Scripts/python.exe -B firmware/camera/tools/run_host_tests.py
& ./.venv/Scripts/python.exe -B -m unittest discover -s tests -p 'test_firmware_contract.py' -v
& ./firmware/lcd_viewer/build.ps1 -HostTestsOnly
```

Camera and contract checks require native `g++` on PATH; Node.js is required
for the browser logic tests. Use a fresh log directory for each retained run.
The server runner uses temporary synthetic data; the manual-review fixture also
creates a uniquely named synthetic directory in the checkout. Keep production
notification credentials out of the test process.

Camera preview/identity compilation commands and pinned model preparation are
in the [camera README](firmware/camera/README.md). Full LCD compilation and
dependency preparation are in the [LCD README](firmware/lcd_viewer/README.md).
The camera identity profile regenerates pinned public model files in its ignored
vendor directory; it must not depend on an untracked manifest copied by itself.

Web requirements are not fully version-locked. Retain the actual dependency
versions and build/test logs when reporting a verified environment. Compilation,
host tests, real inference, hardware acceptance and deployment are separate
results. Flashing, OTA, storage formatting and production operations require
their own target-specific procedure and authorization.
