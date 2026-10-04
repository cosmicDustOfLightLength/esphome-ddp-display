# Design notes and findings

A record of what was built, which problems showed up on real hardware, **why** they happened
and how they were solved — in the order they were found.

Hardware: ESP32-S31 (dual core, 320 MHz, 14 MB PSRAM), ESP-IDF 6.1.0, ESPHome 2026.9.1,
4× HUB75 64×64 in a 2×2 layout (128×128), esp-hub75 driver (PARLIO, clock gating, 8-bit,
double buffer 2 × 670 KB in PSRAM, 62 Hz refresh), WiFi and Ethernet YT8531 (RGMII, 1 Gbit).

---

## 1. Design decisions

| Decision | Why |
|---|---|
| **DDP protocol** instead of a custom one | 10-byte header, offset + push flag = simple frame assembly; works with existing software (xLights, LedFx…) |
| **UDP**, not TCP | for live video, dropping a frame is better than waiting for retransmission |
| **ESPHome external component** | ESPHome is kept — normal screen, Home Assistant, OTA and sensors all keep working |
| **Auto / DDP / Normal modes** + timeout | the panel never gets "stuck" in network mode; static images are possible in DDP mode |
| Integration via **one lambda line** (`if (id(ddp).draw(it)) return;`) | works with any `display`, no driver changes |
| Reception in a **dedicated FreeRTOS task** with a blocking `recv()` | packets are pulled out of lwIP immediately, independent of the ESPHome loop |
| A frame is shown only after **push** | no tearing while a multi-packet frame is arriving |

---

## 2. Problems and their root causes

### 2.1 The image was drawn "line by line, like a CRT"
**Symptom:** visible progressive drawing, black bands on animation.
**Cause:** the first version of `draw()` called `draw_pixel_at()` 4096 times. In esp-hub75,
`set_pixel()` = `draw_pixels(1×1)`, and in single-buffer mode **every** such call runs
`esp_cache_msync()` over the whole DMA buffer (670 KB) — 4096 cache flushes per frame.
**Fix:** a single `it.draw_pixels_at(...)` call for the whole frame (RGB888 with stride) — hub75
converts it in one pass with one flush.

### 2.2 Black stripes with `auto_clear_enabled: true`
**Cause:** ESPHome clears the buffer before the lambda, then the frame is drawn — when drawing is
visible, the black intermediate state shows. **Fix:** `auto_clear_enabled: false` plus
`it.fill(Color::BLACK)` only in the normal-screen branch.

### 2.3 Flickering areas outside the DDP frame (64×64 on a 128×128 display)
**Cause:** with double buffering and no auto clear, each of the two buffers kept *different*
stale content (e.g. the equalizer) in the unused 3/4 of the screen → it flipped back and forth
on every buffer swap. Clearing on every frame made problem 2.4 worse.
**Fix:** `draw()` clears the whole screen only for the **first 2 frames** after DDP becomes
active (once per buffer), then draws only its own area.

### 2.4 ⭐ The main problem: random black stripes on moving content
**Symptom:** a static pattern (`ddp_image.py --pattern`) was perfectly stable, an animation
(`ddp_test.py`) flickered with stripes. Changing `update_interval` (20/40/60 ms) or switching
WiFi → Ethernet did not help.
**Diagnosis:** the static-vs-animated test ruled out the network and DDP (the component only
shows complete frames). What remained was drawing into a buffer that is still being displayed.
**Cause (esp-hub75, PARLIO):** in *loop transmission* mode, `parlio_tx_unit_transmit()` only
swaps the DMA link — the hardware moves to the new buffer **only after the current pass
finishes** (here 343,232 words / 20 MHz ≈ **17.2 ms**). `flip_buffer()` returned immediately,
so ESPHome started drawing the next frame into the buffer the DMA was still scanning. With a
static image the same values are written → invisible; with motion → mixed frames / black
fragments (cached writes evicted to PSRAM while being scanned).
**Fix (esp-hub75 patch):**
- register `parlio_tx_unit_register_event_callbacks()` with `on_buffer_switched` (ISR, IRAM,
  binary semaphore as `user_ctx` — safe with `CONFIG_PARLIO_TX_ISR_CACHE_SAFE`),
- `flip_buffer()`: drop a stale signal → `transmit()` → wait for the semaphore
  (timeout: 2 passes + 10 ms),
- fallback when the callback is unavailable: `vTaskDelay` for one full pass.
Confirmation in the log: `ParlioDma: Buffer switch sync: on_buffer_switched (frame 17161 us)`.
Commit `d67636c` in `cosmicDustOfLightLength/esp-hub75@esp32s31-support`; patch in `patches/`.
It applies to every PARLIO chip in double-buffer mode, not just the S31 → candidate for an
upstream PR.

**Deployment pitfall:** the first flash "didn't work" because the builder compiled a stale
cached copy of the library. Always verify the `Buffer switch sync` line in the boot log.

### 2.5 FPS dropped after a few seconds (28 → 10–16 on WiFi, 2–5 on Ethernet)
**Cause 1 (component):** reception and drawing shared a mutex for the duration of
`draw_pixels` (several ms at 128×128 into PSRAM). The RX task waited, the lwIP socket queue
(default only **6** packets!) overflowed, and since a 128×128 frame is 35 packets sent as a
burst, the push packet was often lost → the whole frame was lost.
**Fix:** **triple buffering** (RX writes to *back*, a complete frame becomes *ready*, drawing
uses *front*); only the index swap happens under the mutex. RX task priority 5 → 10 (above the
ESPHome loop, below lwIP/WiFi/ETH). `CONFIG_LWIP_UDP_RECVMBOX_SIZE: 64`.
Result on WiFi: ~29 fps, ~1010 pkt/s (≈ 35 × 29).

**Cause 2 (Ethernet):** the EMAC has **20 RX DMA buffers of 512 B** (~10 KB) by default. A
~1450 B packet takes 3 buffers → only ~6 packets fit. On gigabit a 35-packet burst arrives
within a fraction of a millisecond → the hardware drops the rest (371 pkt/s instead of ~1000).
**Fix:** `CONFIG_ETH_DMA_BUFFER_SIZE: 1536` (1 packet = 1 buffer, multiple of 64 B),
`CONFIG_ETH_DMA_RX_BUFFER_NUM: 30` (maximum), `CONFIG_LWIP_TCPIP_RECVMBOX_SIZE: 64`.
Cost ~46 KB of internal RAM. Result: ~1009 pkt/s.

**Cause 3 (sender):** sending a whole frame as one burst. **Fix:** the tools spread packets
evenly over part of the frame time (`--spread`, default 0.5). On Windows `time.sleep()` has
1–15 ms resolution, so the end of each wait is a busy-wait (`perf_counter`).

### 2.6 Other observations
- `power_save_mode: none` on WiFi — power save delays and drops incoming UDP.
- Panel `update_interval`: 20 ms was clearly worse (each flip = 670 KB cache writeback + waiting
  for a pass), 40 ms is a good compromise (~25 fps displayed).
- Use `on_redraw: component.update` only with a long `update_interval`; with a short one it
  causes double flips.
- `parlio: FIFO empty interrupt triggered unexpectedly` appears only on reboot after OTA
  (DMA being stopped), never during operation.
- hub75's `min_refresh_rate` is derived from `update_interval` (clamped to 40–200 Hz).

---

## 3. Troubleshooting

| Symptom | Check |
|---|---|
| Static is fine, animation flickers | log: `Buffer switch sync` (esp-hub75 patch), `double_buffer: true`, `auto_clear_enabled: false` |
| Drawing visible line by line | is double buffering really active (log `Double buffering: 2 × … KB`), are you drawing via `draw()` |
| Low FPS, `packets/s` ≈ 35 × sent fps | network is fine → limited by drawing (`update_interval`) |
| Low FPS, `packets/s` too low | packets are lost: `RECVMBOX`, ETH DMA buffers, power save, `--spread` |
| `incomplete frames` grows quickly | same as above — packet loss in the middle of frames |
| Nothing shows up | mode `Normal`? port 4048 / firewall? `width/height` match the sender? |

Sensors: `DDP FPS`, `DDP packets/s`, `DDP incomplete frames`, `DDP active`.
Expected packets/s = `ceil(w*h*3 / 1440) × fps` (128×128 @ 30 → 1050).

---

## TODO

- Loss detection via the DDP sequence number (exact lost-frame counter, including lost push packets).
- Option to drop incomplete frames instead of showing them with stale fragments.
- Answer DDP *query*/status packets so software can discover the device.
- X/Y offset for the frame on the display, multiple areas.
- RGB565 / simple compression (fewer packets).
- Upstream PR of the `flip_buffer()` fix to `esphome-libs/esp-hub75`.
- A full PC application (preview, playlists, screen capture).
