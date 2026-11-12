# LCD Viewer UI Design

This document describes the LCD viewer's current functionality, implementation,
page style, and open-source references. Its scope was approved before creation.
It records existing behavior rather than proposing a redesign. Source inspection
supports the descriptions below; physical hardware acceptance remains separate.

## Purpose and current capabilities

The LCD viewer is an on-demand LAN camera-preview terminal. While viewing is
stopped, it presents Home, Status, and Setup pages. Start preview or a short BOOT
press starts a 60-second viewing session with a full-screen monitor.

The configured target is the Waveshare ESP32-S3-Touch-LCD-2.8 V2, with an ST7789
display, CST3530 touch controller, 320 x 240 landscape output, and PSRAM. The
project uses Arduino-ESP32 3.3.12 and pinned LVGL 8.3.10. These are source/build
settings, not confirmation of the identity or capacity of a connected board.

| Capability | Current implementation |
| --- | --- |
| Startup | Initialize serial, check PSRAM, initialize the display and JPEG buffers, load configuration, start the preview client, and create the UI. Initialization failures are reported over serial. |
| Provisioning | Accept newline-terminated JSON over USB serial at 115200 baud while viewing is stopped. Store Wi-Fi credentials, camera host, pairing token, and ports in NVS, then restart. Credentials are not echoed. |
| Session control | Create, renew, query, and close an authenticated camera-preview session. Viewing lasts 60 seconds; renewal is due every 10 seconds. Continue resets the viewing countdown to 60 seconds from the click. |
| Video reception | Connect directly to the configured LAN camera. Control defaults to HTTP port 80; MJPEG defaults to port 81. Both send Bearer authorization. Control and stream reception use separate FreeRTOS tasks. |
| Parsing and recovery | Parse fragmented and chunked multipart MJPEG, bound headers and JPEG size, check sequence numbers, and recover from connection or session loss. Session generations reject obsolete responses and frames. |
| Decode and display | Use two 128 KiB JPEG buffers and two 320 x 240 RGB565 buffers. Decode into a candidate image and swap only on success, preserving the previous valid picture when decoding fails. |
| Input and backlight | Use BOOT for start/stop and touch for navigation, Start, Continue, and Close. Keep the backlight on while viewing or unconfigured; turn it off after approximately three seconds of configured inactivity. |

The viewer does not provide local recording, playback, audio talkback, face
recognition, cloud video relay, weather, or clock features. Enrollment and identity
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
| [viewer_config.cpp](../config/viewer_config.cpp) | USB JSON provisioning and NVS persistence. |
| [monitor_font_28.c](monitor_font_28.c) | Local 28 px display font derived from the pinned LVGL font. |

Home/Status/Setup and Monitor are sibling LVGL panels on the same active screen.
The view switches their visibility using `PreviewSnapshot.active`; Monitor uses
the same LVGL renderer as the dashboard pages.

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
- Default LVGL styles are removed from the constructed components. Panels are
  non-scrollable; navigation uses explicit buttons rather than swipe gestures.

## Screen inventory and layout

Coordinates below are screen coordinates unless stated otherwise.

### Shared stopped-page structure

Home, Status, and Setup share a 42 px header, a 154 px content area, and a 44 px
bottom navigation bar:

| Region | Bounds | Content |
| --- | --- | --- |
| Header | x=0, y=0, w=320, h=42 | `Camera monitor`, `LOCAL PREVIEW`, and right-aligned Wi-Fi status. |
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
| Unconfigured | `Setup required` | `Configure via USB serial` | `See Setup` | Disabled. |
| Configured, Wi-Fi connected | `Ready` | `Preview is off` | `BOOT to start` | Enabled. |
| Configured, Wi-Fi disconnected | `Wi-Fi offline` | `Waiting for Wi-Fi` | `BOOT to start` | Enabled. |

`Ready` describes local configuration and Wi-Fi readiness. It does not establish
camera reachability or an active stream. Starting while disconnected enters the
existing connection-waiting flow.

### Status

Status has four read-only rows: `Wi-Fi`, `Configuration`, `Preview`, and
`Idle backlight`. Labels are left-aligned, Blue values are right-aligned, and
thin Surface lines separate the first three rows.

This page is accessible only while viewing is stopped, so its Preview value is
always `Stopped`. It is not an active-session statistics page. The backlight row
shows `Off after 3s` when configured or `Stays on for setup` when unconfigured.

### Setup

Setup is an informational page with a Peach `USB setup` heading and these steps:

1. `Connect the USB cable`
2. `Send your setup JSON`
3. `Auto-restart; press BOOT`

It also displays `115200 baud | configure while stopped`. The page contains no
editable configuration form, credential display, or reset control.

### Monitor

The decoded image occupies the full 320 x 240 screen at native size. The decoder
accepts only that image size; no scaling or cropping step is applied. The image
is presented underneath the following overlays:

| Region | Bounds | Appearance and content |
| --- | --- | --- |
| State bar | x=0, y=0, w=320, h=26 | Crust at `LV_OPA_90`; colored state dot/mode on the left, Peach countdown on the right. |
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

- A short BOOT press starts viewing from any stopped-page tab and stops an active
  session. BOOT uses GPIO0 with a 35 ms debounce during normal operation.
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

The backlight remains on during active viewing and while the device is
unconfigured. A configured device turns it off approximately three seconds after
UI initialization or the transition from active to inactive. Switching dashboard
tabs does not restart this timer. BOOT explicitly turns the backlight on and
resets the idle timer.

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

This documentation update is based on source inspection and existing historical
records. It does not constitute a new firmware build, host-test run, off-screen
render, flash operation, or physical acceptance test.

The existing [native renderer](../../tests/render_ui.cpp) exercises the production
view with real LVGL and synthetic RGB565 images. Its scenes cover the dashboard
pages, unconfigured/offline states, first-frame waiting, LIVE, event/enrollment/
identity pauses, Wi-Fi reconnecting, stale frames, errors, long text, session
replacement, and stop. [Controller tests](../../tests/test_preview_ui.cpp) cover
UI actions, BOOT, timing, backlight, and generation handling.

The [render tool](../../tools/render_ui.py) is documented in the LCD README and
writes current results to repository-root `build/lcd_viewer/ui-native/`. Legacy
images under `firmware/lcd_viewer/.build/ui-native/` are historical off-screen
examples with synthetic video content, not physical camera captures or fresh
verification of the current source. Generated screenshots are not required to
read this document.

Physical display colors, touch coordinates, BOOT response, backlight behavior,
sustained streaming, camera-priority recovery, frame rate, latency, and power
still require validation on the actual hardware. Historical compile/test results
must retain their dates and source bindings; they are not evidence of a new run.
