# Monitoring Server

The server receives evidence from the [ESP32 camera](../firmware/camera/README.md),
groups photos into events, runs analysis or manual review, and provides the web
dashboard. The project's GPU deployment target is a Linux cloud server with an
NVIDIA RTX 4090 running Qwen2.5-VL-7B and a LoRA adapter.

The [LCD viewer](../firmware/lcd_viewer/README.md) receives live video directly
from the camera on the local network. Its video stream does not pass through
this server. See the [project guide](../README.md) for the complete architecture
and hardware setup.

## Functions in the System

| Function | What the server does |
| --- | --- |
| Device and account management | Creates administrator/viewer accounts, assigns cameras to viewers, and issues device upload tokens. |
| Evidence ingestion | Validates JPEG uploads and event WAV recordings and saves media with SQLite records. Registered cameras authenticate with per-device tokens; the legacy V1 path has a separate credential setting described below. V2 uploads also contain immutable identity metadata. |
| Event aggregation | Groups images by device, event ID and frame index. Complete groups enter the worker queue; incomplete groups enter review after the collection deadline. |
| GPU assessment | Runs YOLO on each frame. If a person is detected, Qwen with LoRA assesses the ordered image sequence; otherwise the event enters review. Identity events can also receive an assessment of which person performed each action. |
| CPU review | Preserves evidence for a human decision without loading vision models. |
| Dashboard and review | Shows suspicious-event history, images, recordings and pending reviews, restricted to the owning viewer. Keep confirms a suspicious event, Dismiss removes its retained review images, and Retry requeues an eligible complete event. |
| Alarm feedback | Returns the event status when the camera polls. The camera controls its own buzzer and red light. |
| Family members | Sends enrollment/revocation commands for cameras to poll and records confirmed device registrations. Face templates stay on each camera. Automatic family exemption is disabled by default. |
| Audio and notifications | Provides event recording playback, browser-to-camera talkback, and optional Firebase push for registered clients. |

Audio recordings are evidence for playback; the current model worker analyzes
images. Talkback sends browser microphone audio to the camera speaker.

## Cloud Deployment Layout

Run the Web application (`main.py`) and one worker (`worker.py`) as separate
processes on the cloud Linux server. Both must use the same actual
`CAMERA_DATA_DIR`, including its database and media files. Separate containers
must mount that same data directory; matching path strings on different hosts
do not share data.

The worker loads Qwen, LoRA and YOLO from local files in its own process. It does
not call an existing Qwen HTTP API. If another inference service already runs
on the GPU, this worker would load another copy of the model. Qwen uses BF16 on
GPU 0; YOLO defaults to CPU. Measure available GPU memory and five-frame inference
latency in the deployed environment.

The Web service needs an HTTPS reverse proxy reachable by the camera and browser.
The camera verifies the certificate using its built-in public CA bundle and needs
valid network time. Preserve the external `Host`, forward
`X-Forwarded-Proto: https`, and configure Uvicorn to trust forwarded headers only
from the actual proxy. Keep `AUTH_ALLOW_HTTP` unset for this deployment.

Set a private random `UPLOAD_TOKEN` in the Web process environment before exposing
the service. It protects the legacy V1 upload path for unregistered device IDs;
leaving it empty allows those uploads into unassigned storage. Registered cameras
still use their individual device tokens. This server-side setting is separate
from the camera/LCD pairing token.

## Runtime Requirements

- Python 3.12 is the reference environment. The worker uses Linux `fcntl` locking;
  Windows can run the Web application, while a complete local workflow uses WSL
  or Linux.
- [requirements-web.txt](requirements-web.txt) contains the Web/CPU dependencies
  also used by the GPU worker's storage and validation modules.
- GPU mode requires a working CUDA/PyTorch, Transformers and `qwen-vl-utils`
  environment. [requirements-worker.txt](requirements-worker.txt) supplements that
  environment with YOLO and PEFT; it does not install the complete GPU stack.
- Local Qwen2.5-VL-7B, YOLO and compatible LoRA weights are required. The LoRA
  directory must contain `adapter_config.json` and `adapter_model.safetensors`.
  Model loading does not download missing weights.
- Use persistent storage for accounts, media and the queue, with backups that
  preserve their relationship. These private files and model weights stay outside
  Git.

## Start the Services

Run these commands from the repository root in the prepared server environment.
The code already supplies these defaults, so no path exports are needed when
they match the cloud server and both services use the default data directory.

| Optional environment variable | Default when unset |
| --- | --- |
| `CAMERA_DATA_DIR` | `server/ai-data/` in this checkout, resolved from `store.py`. |
| `QWEN_MODEL_PATH` | `/root/autodl-tmp/models/Qwen2.5-VL-7B-Instruct` |
| `QWEN_LORA_PATH` | `/root/autodl-tmp/models/LoRA/inference` |
| `YOLO_MODEL_PATH` | `/root/autodl-tmp/models/yolov8n.pt` |

An environment variable already set in the shell or service configuration
overrides its default. If Web uses a custom `CAMERA_DATA_DIR`, set that same
directory for the worker before starting it. Export model-path variables only
when the model files are in different locations; the defaults are local paths
on the cloud server, not remote model URLs.

Web process, behind the HTTPS reverse proxy:

```bash
export AI_RUNTIME_MODE=gpu_required
: "${UPLOAD_TOKEN:?Set a private random legacy-upload token in the environment first}"
python server/manage_users.py bootstrap
python -m uvicorn main:app --app-dir server --host 127.0.0.1 --port 8000
```

GPU worker, in a second terminal using the same data directory:

```bash
python server/worker.py
```

For manual review without model inference, run `python server/worker.py --cpu-review`
instead of the GPU worker, with the same `CAMERA_DATA_DIR`. Only one
worker may use a data directory at a time. `AI_RUNTIME_MODE` labels the Web health
response; the worker command selects the actual mode. Starting Web alone does not
analyze complete queued events. The root guide also covers a
[local HTTP demonstration](../README.md#startup).

On an empty account database, `bootstrap` saves initial administrator credentials
in `auth/initial-admin.txt` under the active data directory
(`server/ai-data/auth/initial-admin.txt` by default). Read that file privately,
sign in, change the password under **Account**, and remove the initial credential file.
Running `bootstrap` again does not reset existing accounts.

In **Admin**, create a viewer, register the camera's exact device ID, and assign
it to that viewer. Put the generated device token in the camera's local
configuration and set its upload URL to
`https://your-server.example/api/device/upload`, including for identity firmware.
Reassignment changes the device token. The camera/LCD preview token is a separate
credential and is not configured in the server.

## Daily Use and Current Limits

- Sign in as the assigned viewer to inspect events, recordings and reviews.
  **Keep** requires a description. **Retry** requires a complete event with its
  image files; in CPU mode, it returns to manual review.
- Use `/members` with an assigned, online, identity-enabled camera for family
  enrollment. Templates are stored on the device, not backed up by the server.
- Browser talkback needs microphone permission, a secure browser context and a
  camera reporting that it is ready. Firebase push additionally needs configured
  credentials and a registered client.
- The collection window defaults to 30 seconds from the first accepted frame
  (`EVENT_WAIT_SECONDS`). Late new frames are rejected; camera retries use a
  separate 60-second window. Missing frames require review.
- The camera polls results for up to 60 seconds after its pending media clears.
  It stops on `review`, so a later human Keep or a result after that wait does not
  automatically produce a new device alarm.
- `MAX_PENDING` defaults to 200 frame records across collecting, queued,
  processing and review states, approximately 40 complete five-frame events.
  The limit is shared across devices and users. Resolve reviews and monitor disk
  usage and worker health to keep accepting evidence.
- Event-analysis failures send the evidence to review. Optional actor-attribution
  failures preserve the behavior verdict. Initialization failures stop the
  worker, and inference has no independent execution timeout. Worker restart
  requeues interrupted processing. A healthy Web response alone does not confirm
  that inference is progressing.

## Source Map

| Files | Responsibility |
| --- | --- |
| [main.py](main.py), [identity_ingest.py](identity_ingest.py), [store.py](store.py) | Web routes, V1/V2 image ingestion and persistence. |
| [event_queue.py](event_queue.py), [worker.py](worker.py) | Event collection, processing, recovery and inference. |
| [user_auth.py](user_auth.py), [ownership.py](ownership.py) | Accounts, sessions and device ownership. |
| [alarm_policy.py](alarm_policy.py), [device_alarm_api.py](device_alarm_api.py) | Event decisions and device result queries. |
| [member_api.py](member_api.py), [face_registry.py](face_registry.py), [device_commands.py](device_commands.py) | Family members, device registrations and commands. |
| [audio_api.py](audio_api.py), [push_dispatcher.py](push_dispatcher.py) | Recordings, talkback and optional notifications. |

Existing regression checks are collected by [run_tests.py](run_tests.py), with
test transport dependencies in [requirements-test.txt](requirements-test.txt).
Synthetic tests do not establish real-model accuracy, cloud latency or physical
camera alarm behavior.
