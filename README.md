# esphome-ddp-display

**Control an ESPHome display over the network — raw pixels over UDP using the DDP protocol.**
Send an image, an animation or a video from your PC and the ESP shows it on the panel.
You keep ESPHome: your normal screen (clock, sensors, equalizer…) keeps working, and the
mode switches automatically or from Home Assistant.

Tested on: **ESP32-S31** (ESP-IDF 6.1.0, ESPHome 2026.9.1), 4× **HUB75 64×64** panels in a 2×2
layout (128×128), over WiFi and Ethernet (YT8531, RGMII). ~30 fps at 128×128 over WiFi.

---

## Contents
- [Features](#features)
- [How much data is it](#how-much-data-is-it)
- [Installation (ESPHome)](#installation-esphome)
- [Configuration](#configuration)
- [Diagnostic sensors](#diagnostic-sensors)
- [HUB75 — important!](#hub75--important)
- [Recommended network settings](#recommended-network-settings)
- [PC tools](#pc-tools)
- [Your own sender — the protocol in 2 minutes](#your-own-sender--the-protocol-in-2-minutes)
- [Measurements](#measurements)
- [Tests](#tests)
- [Known limitations](#known-limitations)
- [Repository layout](#repository-layout)

Debugging details and all findings: **[docs/NOTES.md](docs/NOTES.md)**.

---

## Features

- Receives RGB888 frames over **UDP / [DDP](http://www.3waylabs.com/ddp/)** (standard port 4048) —
  works with software that outputs DDP (e.g. xLights, LedFx) and with the bundled scripts.
- **3 modes**, switchable from a select in Home Assistant and restored after reboot:
  - **Auto** — shows network frames while they arrive; the normal screen returns after `timeout` of silence,
  - **DDP** — always shows the last network frame (e.g. a static image sent once),
  - **Normal** — plain ESPHome, DDP is ignored.
- A frame is shown only when **complete** (push flag) — no tearing while packets are arriving.
- **Triple buffering**: reception runs in its own FreeRTOS task and never waits for drawing.
- The whole frame is drawn with **one** `draw_pixels_at` call (fast path in drivers such as hub75).
- Frame smaller than the display → drawn in the top-left corner, the rest is black.
- **Diagnostics**: FPS, packets/s, incomplete-frame counter, active state.
- Works with any ESPHome display that has a `lambda` (`display::Display`).

## How much data is it

| Size | Bytes / frame | UDP packets / frame | at 30 fps |
|---|---|---|---|
| 64×64 | 12,288 | 9 | ~3 Mbit/s |
| 128×128 | 49,152 | 35 | ~12 Mbit/s |

A static image is one frame. "Live" video is the same frames sent in a loop.

---

## Installation (ESPHome)

```yaml
external_components:
  - source: github://cosmicDustOfLightLength/esphome-ddp-display@master
    components: [ddp_display]
```

> If you fork or rename the repo, adjust `github://owner/repo@branch`. You can pin a tag or
> commit instead of `main`.

Requirements: **ESP32 with the ESP-IDF framework**, a `network` (WiFi or Ethernet) and any `display`.

## Configuration

```yaml
ddp_display:
  id: ddp
  width: 128          # DDP frame size (required)
  height: 128         # (required)
  port: 4048          # optional, default 4048
  timeout: 5s         # Auto mode: normal screen returns after this long without frames (0s = never)
  # on_redraw:        # optional: fired on a new frame / mode change
  #   - component.update: my_display   # ONLY if the display has a long update_interval (e.g. 1s)

display:
  - platform: ...
    id: my_display
    update_interval: 40ms
    lambda: |-
      if (id(ddp).draw(it)) return;   // <- the only required line, at the top of the lambda
      // ... normal screen ...

select:                                 # mode switch in Home Assistant
  - platform: template
    name: "Display mode"
    options: ["Auto", "DDP", "Normal"]
    initial_option: "Auto"
    optimistic: true
    restore_value: true
    on_value:
      - lambda: id(ddp).set_mode(x);
```

Ready-to-use files: [`examples/minimal.yaml`](examples/minimal.yaml),
[`examples/hub75-esp32-s31.yaml`](examples/hub75-esp32-s31.yaml) (full config with sensors).

### Lambda API

| Method | Description |
|---|---|
| `bool draw(it)` | Draws the DDP frame if active; returns `true` when it did |
| `bool is_active()` | Whether the network image is currently shown |
| `set_mode("Auto" \| "DDP" \| "Normal")` | Change mode (case-insensitive) |
| `const char *get_mode_str()` | Current mode |
| `float get_fps()` | Frames / s (measured every 1 s) |
| `float get_packet_rate()` | UDP packets / s |
| `uint32_t get_frame_count()` / `get_packet_count()` | Counters since boot |
| `uint32_t get_incomplete_count()` | Frames shown with missing packets (since boot) |

### Diagnostic sensors

Optional, but very useful for checking the network and tuning. Add to your YAML:

```yaml
binary_sensor:
  - platform: template
    name: "DDP active"
    lambda: return id(ddp).is_active();

sensor:
  - platform: template
    name: "DDP FPS"
    unit_of_measurement: fps
    accuracy_decimals: 1
    update_interval: 5s
    lambda: return id(ddp).get_fps();

  - platform: template
    name: "DDP packets/s"
    unit_of_measurement: pkt/s
    accuracy_decimals: 0
    update_interval: 5s
    lambda: return id(ddp).get_packet_rate();

  - platform: template
    name: "DDP incomplete frames"
    accuracy_decimals: 0
    state_class: total_increasing
    update_interval: 5s
    lambda: return id(ddp).get_incomplete_count();
```

| Sensor | Meaning |
|---|---|
| **DDP active** | `on` = the network image is on screen (mode DDP, or Auto while frames arrive) |
| **DDP FPS** | complete frames received per second (measured over the last second) |
| **DDP packets/s** | UDP packets received per second |
| **DDP incomplete frames** | frames shown with at least one missing packet — **cumulative since boot**, watch how fast it grows, not the value |

**How to read them** — packets per frame = `ceil(width × height × 3 / 1440)`
(64×64 → 9, 128×128 → 35):

- `packets/s ≈ packets_per_frame × FPS` and FPS ≈ what you send → everything arrives, all good.
  Example: 829 pkt/s ÷ 35 ≈ 23.7 fps → consistent; the sender is the limit.
- `packets/s` lower than `packets_per_frame × sent fps` → packets are lost on the way
  (see [Recommended network settings](#recommended-network-settings)).
- `incomplete frames` grows by more than a few per minute → packet loss in the middle of frames
  (same fixes). A slowly growing counter at ~1–3% loss is normal on WiFi.
- FPS lower than `packets/s ÷ packets_per_frame` → push packets get lost (frames merge).

Compare with what the sender prints (`ddp_test.py` / `ddp_video.py` show their actual send fps).

---

## HUB75 — important!

HUB75 panels (`platform: hub75`, esp-hub75 library) need **three settings**:

```yaml
display:
  - platform: hub75
    double_buffer: true          # otherwise you can see the drawing line by line
    auto_clear_enabled: false    # clearing every frame = black stripes
    update_interval: 40ms        # 20 ms was clearly worse; 40 ms ≈ 25 fps
    lambda: |-
      if (id(ddp).draw(it)) return;
      it.fill(Color::BLACK);     // with auto_clear off, clear manually (normal mode)
      ...
```

**On chips with PARLIO (ESP32-P4/S31/C6) esp-hub75 needs a fix**: the original `flip_buffer()`
does not wait until the hardware actually switches buffers (~17 ms), so the next frame is drawn
into the buffer that is still being displayed → **flickering black stripes on moving content**
(static images look fine). The fix registers ESP-IDF's `on_buffer_switched` callback and waits
for the switch.

- available in the fork: [`cosmicDustOfLightLength/esp-hub75` @ `esp32s31-support`](https://github.com/cosmicDustOfLightLength/esp-hub75/commits/esp32s31-support/) (commit `d67636c`),
- as a patch: [`patches/esp-hub75-0001-parlio-wait-for-buffer-switch.patch`](patches/) (`git am`).

Using the fork in ESPHome (a YAML entry takes precedence over the version added by `hub75`):

```yaml
esp32:
  framework:
    type: esp-idf
    components:
      - name: esphome/esp-hub75
        source: https://github.com/cosmicDustOfLightLength/esp-hub75.git
        ref: esp32s31-support
        path: components/hub75
```

**Verify:** the boot log must contain
`ParlioDma: Buffer switch sync: on_buffer_switched (frame ~17000 us)`.
If it is missing, the build used a stale copy of the library → clean the build
(`esphome clean` / delete the library cache).

---

## Recommended network settings

```yaml
esp32:
  framework:
    sdkconfig_options:
      CONFIG_LWIP_UDP_RECVMBOX_SIZE: "64"     # UDP socket queue (default is only 6!)
      CONFIG_LWIP_TCPIP_RECVMBOX_SIZE: "64"
      # Ethernet (internal EMAC) only: default 20×512 B ≈ 6 packets -> bursts get dropped
      CONFIG_ETH_DMA_BUFFER_SIZE: "1536"
      CONFIG_ETH_DMA_RX_BUFFER_NUM: "30"      # ~46 KB RAM

wifi:
  power_save_mode: none                       # power save delays/drops incoming UDP
```

Changes to `sdkconfig_options` require a **clean build**.

On the sender side: **spread packets over time** instead of sending a whole frame as one burst
(especially on gigabit Ethernet, where 35 packets arrive within a fraction of a millisecond).
The bundled scripts do this by default (`--spread 0.5` = packets spread over half the frame time).

---

## PC tools

In [`tools/`](tools/). `pip install -r tools/requirements.txt` (or only what you need).

| Script | Purpose | Dependencies |
|---|---|---|
| `ddp_test.py` | animated gradient / solid color, prints the actual send fps | none |
| `ddp_image.py` | static image (PNG/JPG…) or a test pattern | Pillow (for images) |
| `ddp_video.py` | YouTube video (yt-dlp) or a video file, in real time | opencv-python, numpy, yt-dlp |

```bash
# animation test, 128x128 @ 30 fps
python tools/ddp_test.py 192.168.1.50 --size 128x128
python tools/ddp_test.py 192.168.1.50 --solid ff0000 --size 128x128

# image / test pattern (re-sent every 2 s by default so Auto mode does not time out)
python tools/ddp_image.py 192.168.1.50 picture.png --size 128x128 --fit cover
python tools/ddp_image.py 192.168.1.50 --pattern --size 128x128
python tools/ddp_image.py 192.168.1.50 logo.png --once        # once (use mode "DDP")

# video
python tools/ddp_video.py 192.168.1.50 "https://www.youtube.com/watch?v=..."
python tools/ddp_video.py 192.168.1.50 video.mp4 --loop --fit contain --brightness 0.6
```

`ddp_video.py` downloads video only (no audio, ≤480p, H.264 preferred) into `ddp_cache/`,
scales it (`--fit cover|contain|stretch`), sends at most `--fps 30` and skips frames when it
falls behind. Only download content you have the right to use (see YouTube's terms).

**"Static pattern is fine, animation flickers"** → see [HUB75 — important!](#hub75--important).

---

## Your own sender — the protocol in 2 minutes

A frame is an RGB888 buffer (row by row, `width*height*3` bytes) split into UDP packets
(≤ 1440 data bytes each), each with a 10-byte DDP header:

| Byte | Value |
|---|---|
| 0 | flags: `0x40` (version 1), **`| 0x01` = push** on the last packet of the frame |
| 1 | sequence number (any, 0–15) |
| 2 | data type (e.g. `0x0B` = RGB 8-bit; ignored) |
| 3 | device ID: `1` (or `255`) |
| 4–7 | offset into the buffer (bytes, big-endian) |
| 8–9 | data length (big-endian) |

```python
import socket, struct

def send_frame(sock, addr, rgb: bytes, chunk=1440):
    for off in range(0, len(rgb), chunk):
        data = rgb[off:off + chunk]
        push = 0x01 if off + len(data) >= len(rgb) else 0
        sock.sendto(struct.pack(">BBBBIH", 0x40 | push, 0, 0x0B, 1, off, len(data)) + data, addr)

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
send_frame(s, ("192.168.1.50", 4048), bytes([255, 0, 0]) * (128 * 128))   # red screen
```

Ignored: *query/reply/storage* packets and IDs other than 1/255. The timecode header variant
(flag `0x10`, 14 bytes) is supported.

---

## Measurements

ESP32-S31 @ 320 MHz, 128×128, `update_interval: 40ms`, sender: `ddp_test.py --spread 0.5`.

| Link | Before the fixes | After the fixes |
|---|---|---|
| WiFi (-55 dBm) | 28 → 10–16 fps after a few s, black stripes | **~29 fps**, ~1010 pkt/s, stable |
| Ethernet 1 Gbit (YT8531) | 2–5 fps | ~1009 pkt/s (packets arrive), 22–29 fps |

The panel refreshes in hardware at 62 Hz (BCM, 8-bit, 20 MHz); one DMA buffer pass ≈ 17.2 ms.

---

## Tests

```bash
./tests/run_tests.sh
```

- **C++**: the component is compiled on a PC against minimal ESPHome/lwIP/FreeRTOS stubs
  (`tests/host/stubs`), using real UDP sockets on 127.0.0.1 — DDP parser, modes, timeout,
  incomplete frames, clearing outside the DDP area, and 60 bursty 128×128 frames while drawing
  is deliberately slow (0 lost, 0 torn).
- **Python**: all 3 tools — packets reassembled into frames byte-exact, `--spread` pacing,
  image scaling, real-time video playback (60 → 30 fps).

These tests do not replace an ESPHome build on real hardware — that is checked with `esphome run`.

## Known limitations

- A frame with a **lost packet** is still shown; the missing part comes from an earlier frame
  (triple buffer). At ~3% loss this is practically invisible.
- A lost **last** packet (push) merges the frame with the next one — not counted as "incomplete".
- With the fix, `flip_buffer()` blocks the ESPHome loop for up to ~17 ms → possible
  `hub75 took a long time` warnings (harmless).
- `parlio: FIFO empty interrupt triggered unexpectedly` on reboot after OTA — happens while DMA
  is being stopped, does not affect operation.
- RGB888 only, no compression. DDP *query* packets are not answered.

Ideas for later: [docs/NOTES.md#todo](docs/NOTES.md#todo).

## Repository layout

```
components/ddp_display/   ESPHome external component
  __init__.py             YAML schema / codegen
  ddp_display.h/.cpp      UDP receive, triple buffer, modes, drawing
  ddp_protocol.h          DDP parser (no dependencies, testable on a PC)
examples/                 minimal.yaml, hub75-esp32-s31.yaml
tools/                    ddp_test.py, ddp_image.py, ddp_video.py, requirements.txt
tests/                    run_tests.sh, C++ (host/) and Python tests
patches/                  esp-hub75 fix (PARLIO flip_buffer sync)
docs/NOTES.md             findings, root causes, troubleshooting
```

## License

MIT — see [LICENSE](LICENSE).
