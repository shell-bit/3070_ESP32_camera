# LCD Viewer UI Design

This document describes the LCD viewer's current functionality, implementation,
page style, and open-source references. Its scope was approved before creation.
It records existing behavior rather than proposing a redesign. Source inspection
supports the descriptions below; physical hardware acceptance remains separate.

## Purpose and current capabilities

The LCD viewer is an on-demand LAN camera-preview terminal with an independent
Wi-Fi clock. While viewing is stopped, it presents Home, Status, and Setup pages.
After both Wi-Fi and camera are configured, Start preview or a short BOOT press
starts a 60-second viewing session with a full-screen monitor. Wi-Fi alone is
sufficient for network time; camera setup can be completed later.

The configured target is the Waveshare ESP32-S3-Touch-LCD-2.8 V2, with an ST7789
display, CST3530 touch controller, 320 x 240 landscape output, and PSRAM. The
project uses Arduino-ESP32 3.3.12 and pinned LVGL 8.3.10. These are source/build
settings, not confirmation of the identity or capacity of a connected board.

| Capability | Current implementation |
| --- | --- |
| Startup | Initialize serial, check PSRAM, initialize the display and JPEG buffers, load configuration, start the preview client, and create the UI. Initialization failures are reported over serial. |
| Provisioning | Touch Setup independently saves Wi-Fi or camera host/token/ports and preserves the other saved group. Each section has Save & restart with checked NVS writes and previous-record recovery. Either group can be saved first. Idle USB JSON at 115200 baud remains a whole-record replacement outside draft/scan ownership. Credentials are not echoed. |
| Clock | Nonblocking SNTP synchronization, Beijing time (UTC+8), seconds in Home and Monitor, date/status in Connection info. Unsynchronized startup shows placeholders; synchronized time continues during Wi-Fi loss. |
| Session control | Create, renew, query, and close an authenticated camera-preview session. Viewing lasts 60 seconds; renewal is due every 10 seconds. Continue resets the viewing countdown to 60 seconds from the click. |
| Video reception | Connect directly to the configured LAN camera. Control defaults to HTTP port 80; MJPEG defaults to port 81. Both send Bearer authorization. Control and stream reception use separate FreeRTOS tasks. |
| Parsing and recovery | Parse fragmented and chunked multipart MJPEG, bound headers and JPEG size, check sequence numbers, and recover from connection or session loss. Session generations reject obsolete responses and frames. |
| Decode and display | Use two 128 KiB JPEG buffers and two 320 x 240 RGB565 buffers. Decode into a candidate image and swap only on success, preserving the previous valid picture when decoding fails. |
| Input and backlight | BOOT starts/stops viewing when preview configuration is complete, otherwise it only wakes the display. It acts as Back inside Setup. After 120 seconds without input, Setup discards its draft and returns Home. The 30-second idle cutoff applies whenever Wi-Fi is saved, including Wi-Fi-only operation; dashboard tab touches do not restart it. |

The viewer does not provide local recording, playback, audio talkback, face
recognition, cloud video relay, or weather. Enrollment and identity
processing appear only as camera-reported preview-pause reasons.

See the [LCD README](../../README.md) for build and provisioning instructions and
[third-party sources](../../THIRD_PARTY.md) for dependency and hardware details.

## Implementation and ownership

```text
Camera MJPEG
    -> incoming JPEG parser buffer
    -> latest complete pending JPEG
    -> successful RGB565 decode
    -> LVGL image and overlays
    -> synchronous SPI display transfer
```

The Arduino sketch task owns LVGL calls, touch processing, JPEG decoding, and
display transfers. Network tasks exchange state and frame data with this task;
they do not call LVGL. The receiver drops a completed frame if the decoder owns
the pending buffer instead of building a frame queue. LVGL's flush is acknowledged
only after the synchronous SPI transfer completes.

| Source | Responsibility |
| --- | --- |
| [lcd_viewer.ino](../../lcd_viewer.ino) | Startup and main-loop composition; poll serial provisioning only while inactive. |
| [monitor_view.cpp](monitor_view.cpp) | LVGL pages, visual tokens, navigation, state presentation, and frame image descriptor. |
| [preview_ui.cpp](preview_ui.cpp) | Dispatch Start/Extend/Stop, process BOOT, manage backlight timing, decode frames, and clear obsolete images. |
| [preview_client.cpp](../preview/preview_client.cpp) | Wi-Fi, HTTP session control, independent MJPEG reception, frame handoff, and state snapshots. |
| [preview_state.h](../preview/preview_state.h) | Snapshot consumed by the view: activity, configuration, Wi-Fi, generation, remaining time, sequence, frame age, and error/state text. |
| [session_clock.h](../preview/session_clock.h) and [session_state.h](../preview/session_state.h) | Viewing deadline, renewal timing, freshness, and session-generation checks. |
| [mjpeg_parser.cpp](../preview/mjpeg_parser.cpp) | Bounded multipart/chunked parsing and complete-frame delivery. |
| [jpeg_display.cpp](../board/jpeg_display.cpp) | Exact-size JPEG validation, RGB565 decoding, and successful-image buffer swap. |
| [board_port.cpp](../board/board_port.cpp) | Display, touch, BOOT debounce, backlight, and LVGL timing/flush integration. |
| [viewer_config.h](../config/viewer_config.h) and [viewer_config.cpp](../config/viewer_config.cpp) | Separate Wi-Fi/camera readiness, optional-group stored validation, USB JSON replacement and NVS persistence with unchanged 360-byte layout. |
| [setup_controller.cpp](../config/setup_controller.cpp) | Section-scoped in-memory draft, field edits, explicit secret keep/replace, preserving the other group on save, cancel, and inactivity. |
| [setup_view.cpp](setup_view.cpp) | On-demand Setup pages, bounded network list, keyboard, discard confirmation, connection information, and USB help. |
| [setup_scan.cpp](../preview/setup_scan.cpp) | Scan ownership, operation IDs, timeout/cancellation, capped results, and cleanup through the control worker's radio adapter. |
| [viewer_clock.cpp](../clock/viewer_clock.cpp) | SNTP startup/callback and copied wall-clock snapshot; independent of monotonic session timing. |
| [monitor_font_28.c](monitor_font_28.c) | Local 28 px display font derived from the pinned LVGL font. |

Home/Status and Monitor share the active LVGL screen. Setup hides those panels
and creates only the current Setup page/editor on demand. The view switches
Monitor visibility using `PreviewSnapshot.active`; all pages use the same LVGL
renderer and the unchanged 48 KiB allocator.

## Overall visual style

The UI uses the Catppuccin Mocha dark palette. Dark header and footer bands define
the page structure. Peach emphasizes the main status, countdown, and Continue
action. Blue identifies primary actions, the camera mark, and navigation
selection. Green communicates normal operation, yellow indicates waiting or
pausing, and red indicates errors and the Close action.

The layout is fixed to 320 x 240 landscape. It favors clear text hierarchy, large
touch targets, restrained separators, and a few rounded elements. The current
implementation has no page-transition animations, gradients, shadows, or
per-frame grayscale filter. Interface copy is English.

### Color tokens

| Token | sRGB value | Use |
| --- | --- | --- |
| Base | `#1E1E2E` | Page background. |
| Crust | `#11111B` | Headers, navigation, monitor bars, and notices. |
| Surface | `#313244` | Separators, pressed navigation, and disabled Start. |
| Text | `#CDD6F4` | Primary text. |
| Muted | `#A6ADC8` | Secondary text and disconnected Wi-Fi indicator. |
| Blue | `#89B4FA` | Start button, camera mark, selected-tab underline, and status values. |
| Peach | `#FAB387` | Home hero, selected-tab text, countdown, and Continue. |
| Green | `#A6E3A1` | Connected Wi-Fi and LIVE. |
| Yellow | `#F9E2AF` | CONNECTING and PAUSED. |
| Red | `#F38BA8` | ERROR and Close. |

These constants are defined in `monitor_view.cpp` and converted by
`lv_color_hex` to the configured native RGB565 representation. Actual panel
color appearance requires hardware validation.

### Typography and components

- Montserrat 12 px: secondary text, instructions, metadata, and status rows.
- Montserrat 14 px: page title, navigation, buttons, monitor mode, and countdown.
- Montserrat 16 px: Setup heading and monitor notice titles.
- Local Montserrat 28 px: Home's main status text.
- Major buttons and navigation targets are 44 px high.
- Start has a 6 px radius, a blue normal fill, `#74A8FA` pressed fill, and Surface
  disabled fill. Notice panels have a 7 px radius.
- The camera mark is a blue outlined rounded rectangle and polyline drawn with
  LVGL primitives. It does not require a bitmap asset.
- Default LVGL styles are removed from the constructed components. Dashboard
  panels are non-scrollable. Setup lists can scroll; navigation uses explicit
  buttons. The compact keyboard uses smaller keys than the major 44 px actions.

## Screen inventory and layout

Coordinates below are screen coordinates unless stated otherwise.

### Shared stopped-page structure

Home and Status share a 42 px header, a 154 px content area, and a 44 px
bottom navigation bar:

| Region | Bounds | Content |
| --- | --- | --- |
| Header | x=0, y=0, w=320, h=42 | `Camera monitor`, `LOCAL PREVIEW`, right-aligned time, and Wi-Fi status beneath the time. |
| Content | x=0, y=42, w=320, h=154 | The selected page. |
| Navigation | x=0, y=196, w=320, h=44 | Home, Status, and Setup with widths 106, 107, and 107 px. |

The selected navigation label is Peach with a 3 px Blue underline. Other labels
use Text; a pressed navigation button uses Surface.

### Home

Home contains the camera mark, a 28 px Peach status, a secondary explanation,
`60s sessions`, and a context hint. The blue `Start preview` button is positioned
at x=14, y=142 with a size of 292 x 44 px and a centered play symbol/text group.

| Condition | Main status | Explanation | Hint | Start |
| --- | --- | --- | --- | --- |
| Wi-Fi absent, with or without a saved camera | `Setup required` | `Configure Wi-Fi` | `Open Setup` | Disabled. |
| Wi-Fi saved and connected, camera absent | `Wi-Fi online` | `Camera not set (optional for time)` | `BOOT to wake` | Disabled. |
| Wi-Fi saved but disconnected, camera absent | `Wi-Fi offline` | `Camera not set (optional for time)` | `BOOT to wake` | Disabled. |
| Configured, Wi-Fi connected | `Ready` | `Preview is off` | `BOOT to start` | Enabled. |
| Configured, Wi-Fi disconnected | `Wi-Fi offline` | `Waiting for Wi-Fi` | `BOOT to start` | Enabled. |

`Ready` describes local configuration and Wi-Fi readiness. It does not establish
camera reachability or an active stream. Starting while disconnected enters the
existing connection-waiting flow.

### Status

Status has five read-only rows: `Wi-Fi`, `Wi-Fi setup`, `Camera setup`, `Preview`,
and `Idle backlight`. Labels are left-aligned, Blue values are right-aligned,
and thin Surface lines separate the rows. Wi-Fi reports the actual connection;
the two setup rows independently show `Saved` or `Not set`.

This page is accessible only while viewing is stopped, so its Preview value is
always `Stopped`. It is not an active-session statistics page. The backlight row
shows `Off after 30s` when Wi-Fi is saved or `Stays on for setup` otherwise.

### Setup

Setup is a focused flow with Wi-Fi, Camera, Connection info, and USB setup entries.
It retains the dark Base/Crust palette, Peach headings, Blue actions, and muted
helper text. Full-screen editors make room for a text field and compact ASCII
keyboard; passwords and pairing tokens are masked with no last-character reveal.
Saved secrets are never prefilled in a widget. Keep retains the original secret,
Replace accepts new nonempty text, and Open network explicitly clears a password.

Wi-Fi offers Refresh and manual SSID entry for hidden networks. The asynchronous
list contains at most 16 strongest distinct network/security pairs, with signal,
security, and an access-point suffix. Names outside the font repertoire are
escaped for display but retain their original bytes when selected. Scanning
temporarily disconnects LCD Wi-Fi and then reconnects with the boot configuration.
Pending changes are identified separately from live connection information.

Camera edits the host and pairing token; Advanced contains the two ports. The
menu has only Back in its footer. Wi-Fi and Camera each have Back and Save &
restart in a 44 px footer at y=196: Back is 114 px wide; Save starts at x=116
and is 204 px wide. Wi-Fi Scan is in the content area; Camera Advanced and the
saved-configuration Start shortcut share that row. Start is disabled for dirty
drafts, incomplete preview configuration, or scan ownership.

The flow selects one editable section at a time. Save copies only that section
into the saved record and restarts after verification. Camera Save requires a
host and a token of at least 24 characters; Wi-Fi Save does not require camera
fields and preserves any existing camera token, including a legacy short token.
Validation or storage errors leave edits available for correction. Back from a
changed section asks to discard it; Keep editing returns to that section, while
Discard returns to the menu without a write. BOOT follows Back and never starts
a preview while Setup owns input, including the menu. A 120-second inactivity
deadline clears edits/secrets and cancels scanning before Home idle policy resumes.

Connection info shows the live LCD network, IP and signal, and date/time status.
USB setup retains the 115200-baud JSON recovery instructions;
entering it releases the draft (with confirmation when changed) and drains partial
input before accepting a fresh complete JSON line. All four string keys remain
required: paired empty camera fields explicitly clear that group, and paired
empty Wi-Fi fields omit Wi-Fi. This replacement behavior differs from the
touchscreen's scoped Save. It does not expose credentials.

The clock shows `HH:MM:SS` in UTC+8. Before synchronization it shows `--:--:--`.
After synchronization it continues locally while offline, using muted text for
offline holdover. The Monitor clock sits between mode and countdown. Clock updates
do not reset, extend, or replace the existing monotonic preview deadlines.

### Monitor

The decoded image occupies the full 320 x 240 screen at native size. The decoder
accepts only that image size; no scaling or cropping step is applied. The image
is presented underneath the following overlays:

| Region | Bounds | Appearance and content |
| --- | --- | --- |
| State bar | x=0, y=0, w=320, h=26 | Crust at `LV_OPA_90`; colored state dot/mode on the left, clock in the center, Peach countdown on the right. |
| Frame metadata | x=0, y=178, w=320, h=18 | Crust at `LV_OPA_80`; frame sequence on the left, age on the right. |
| Actions | x=0, y=196, w=320, h=44 | Opaque Crust; Peach `Continue 60s` over 192 px and Red `Close` over 128 px, separated by a thin line. |

The normal LIVE view leaves a 320 x 152 px region, y=26 through y=177, uncovered
by the bars. Full-screen therefore refers to the native-size image underneath
the controls; it does not mean unobstructed video. Dashboard navigation is hidden
while viewing is active.

Waiting, pause, and error notices appear between the top bar and metadata. The
notice is 266 px wide at x=27, has a 7 px radius, and uses Crust at opacity
242/255. Its base height is 58 px. Error details wrap within a bounded area;
the notice can grow to 132 px high. The notice states `Last frame shown.` when a
picture is retained or `No picture yet.` before the first usable frame.

## Interaction and state rules

### Navigation and actions

- A short BOOT press starts viewing from Home/Status when both groups are saved
  and stops an active session. With incomplete preview configuration it only wakes
  the screen, without sending a Start command.
  Within Setup it acts as Back and consumes the press. BOOT uses GPIO0 with a
  35 ms debounce during normal operation.
- Touch Start requires a configured, inactive viewer. An unconfigured BOOT press
  cannot start a session either.
- `Continue 60s` resets the viewing deadline to 60 seconds from the click. It
  does not add 60 seconds to the remaining duration.
- Connection waiting and pause time count toward the viewing deadline.
- Close, BOOT stop, and automatic expiry return to Home.
- Countdown text rounds remaining milliseconds up to whole seconds.

### Monitor state precedence

The first matching row wins, matching `previewMode()` in `monitor_view.cpp`:

| Priority | Condition | Mode | Notice |
| --- | --- | --- | --- |
| 1 | Wi-Fi disconnected | `CONNECTING` without a picture; `PAUSED` with one | `Wi-Fi reconnecting` |
| 2 | Camera reports `paused_for_event` | `PAUSED` | `Capturing event` |
| 3 | Camera reports `paused_for_enrollment` | `PAUSED` | `Member enrollment` |
| 4 | Camera reports `paused_for_identity` | `PAUSED` | `Identity processing` |
| 5 | Camera state is `error` | `ERROR` | `Camera error`, with detail when available. |
| 6 | Client error text is nonempty | `ERROR` | `Reconnecting`, with detail. |
| 7 | No successfully decoded picture is displayed | `CONNECTING` | `Connecting to camera` |
| 8 | Displayed frame is stale | `PAUSED` | `Waiting for a fresh frame` |
| 9 | A fresh picture exists and none of the above apply | `LIVE` | No notice. |

CONNECTING and PAUSED use Yellow, ERROR uses Red, and LIVE uses Green. Text and
notice content accompany color so that status is not conveyed by color alone.

### Frame lifetime and metadata

`Frame` is the displayed frame's sequence number. `Age` is local elapsed time
since that frame was received, with its receive timestamp promoted to displayed
state only after successful decoding. It is not end-to-end camera latency.
The stale threshold is at least 1000 ms. Repeated or older sequence numbers do
not make an old displayed frame fresh again.

Pauses and recoverable errors can retain the current session's last good image.
Stopping or changing session generation clears the previous picture. A failed
decode cannot replace the previous valid image, and a generation change during
decoding prevents the obsolete result from being displayed. Before the first
picture, metadata reads `Waiting for first frame` with no age value.

Readouts normally update at 100 ms intervals. State transitions, user actions,
and the first picture bypass that text-update throttle. This UI update interval
is not a measured video frame rate.

### Backlight behavior

The backlight remains on during active viewing, while a Setup flow owns input,
and while Wi-Fi has not been configured. A device with saved Wi-Fi turns it off
approximately 30 seconds after UI initialization or the transition from
active to inactive, including Wi-Fi-only operation. Switching dashboard
tabs does not restart this timer. BOOT explicitly turns the backlight on and
resets the idle timer. Setup has a separate finite 120-second inactivity policy;
exiting or timing out restarts the normal idle timer. The timer measures time
from those reset events, not time since any touch interaction.

Backlight-off does not put the microcontroller into deep sleep. Wi-Fi remains
configured for automatic reconnect and modem sleep. There is no general
touch-to-wake handler: touch processing continues, and activating an enabled
Start button can begin viewing and thereby turn on the backlight.

## Open-source design references and reused resources

| Project or resource | Relationship to this UI |
| --- | --- |
| [nicholaswilde/cyd-weather-station](https://github.com/nicholaswilde/cyd-weather-station) | Direct visual inspiration: dark header/footer bands, Peach emphasis, Blue actions/navigation, and text hierarchy. The historical design pins [revision 444c9d6f1f54eeb31d416522c07bc03d02e90641](https://github.com/nicholaswilde/cyd-weather-station/tree/444c9d6f1f54eeb31d416522c07bc03d02e90641) and records review of four landscape screenshots and `ui.cpp`. |
| [Catppuccin](https://github.com/catppuccin/catppuccin) | Palette source. The current constants use the Mocha color scheme. |
| [LVGL 8.3.10](https://github.com/lvgl/lvgl/tree/v8.3.10) | Actual graphics dependency for objects, labels, buttons, images, input events, opacity, and font rendering. The pinned copy comes from the official Waveshare V2 archive. |
| [Waveshare V2 examples](https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.8/ESP32-S3-Touch-LCD-2.8-V2-Demo.zip) | Board-driver reference for the pin map, ST7789 initialization, and CST3530 touch protocol. This is hardware adaptation provenance, not the visual-layout source. |
| Montserrat and Font Awesome 5, through pinned LVGL | Font and symbol resources. `monitor_font_28.c` copies the pinned LVGL-generated glyph and kerning tables with local include/guard/symbol changes. See [font provenance and licenses](../../THIRD_PARTY.md#local-28-px-display-font). |

The historical implementation record identifies the monitoring layout and camera
mark as independently implemented. The weather project's visual style was
adapted to monitoring; its weather, clock, driver settings, and other features
were not automatically adopted. The local 44 px touch targets also differ from
the 35 px navigation described in the historical reference comparison.

The full workspace contains these supporting records; they may be absent from
an LCD-only clone:

- [Approved monitor UI design](../../../../docs/superpowers/specs/2026-11-11-lcd-monitor-ui-design.md): visual reference revision and intended adaptation.
- [UI implementation record](../../../../docs/development/lcd-ui-implementation.md): independent layout/camera-mark attribution and dated verification.
- [LCD architecture record](../../../../docs/development/lcd-viewer-architecture.md): source/tests/scripts/dependency organization also inspired by the weather project.

Dependency pins, notices, and licenses remain maintained in
[THIRD_PARTY.md](../../THIRD_PARTY.md).

## Verification and evidence boundaries

This document describes source behavior. Current software verification belongs
to source-bound build/native manifests and the approved implementation plan;
neither this document nor host rendering constitutes physical acceptance.

The existing [native renderer](../../tests/render_ui.cpp) exercises the production
view with real LVGL and synthetic RGB565 images. Its scenes cover the dashboard
pages, unconfigured/offline states, first-frame waiting, LIVE, event/enrollment/
identity pauses, Wi-Fi reconnecting, stale frames, errors, long text, session
replacement, stop, and a basic current Setup-menu snapshot. The retained inventory
contains 16 scenes. [Controller tests](../../tests/test_preview_ui.cpp) cover preview
actions, BOOT, session expiry, backlight, and generation handling. Dedicated
editor/scan/save/clock/Wi-Fi-only cases and feature memory cycling were removed
by the user-approved test cleanup; those features remain implemented but are no
longer covered by the retained harness. The fixture inventory is maintained in
[ui_fixtures.json](../../tests/ui_fixtures.json).

The [render tool](../../tools/render_ui.py) is documented in the LCD README and
writes current results to repository-root `build/lcd_viewer/ui-native/`. The
legacy `firmware/lcd_viewer/.build/ui-native/` path is retired with the duplicate
SDK cleanup. Its historical images were off-screen examples with synthetic video
content, not physical camera captures or fresh verification of the current source.
Use the current source-bound renderer output; generated screenshots are not
required to read this document.

Physical display colors, touch coordinates, BOOT response, backlight behavior,
sustained streaming, camera-priority recovery, frame rate, latency, and power
still require validation on the actual hardware. Historical compile/test results
must retain their dates and source bindings; they are not evidence of a new run.
