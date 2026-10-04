#!/usr/bin/env python3
"""Minimal DDP test sender - animated gradient or a solid color (no dependencies).

  python ddp_test.py 192.168.1.50                            # animated gradient, 64x64 @ 30 fps
  python ddp_test.py 192.168.1.50 --size 128x128             # 128x128
  python ddp_test.py 192.168.1.50 --solid ff0000             # one solid-color frame, then exit
  python ddp_test.py 192.168.1.50 --size 128x128 --spread 0  # send each frame as one burst

Prints the actual send rate once per second. Ctrl+C to stop.
"""
import argparse
import math
import socket
import struct
import time

MAX_PAYLOAD = 1440  # fits in a single UDP packet over WiFi / Ethernet


def wait_until(t):
    # time.sleep() on Windows has 1-15 ms resolution - busy-wait the last 2 ms
    while True:
        left = t - time.perf_counter()
        if left <= 0:
            return
        if left > 0.002:
            time.sleep(left - 0.002)


def send_frame(sock, addr, rgb: bytes, seq: int, spread_s: float = 0.0):
    """Send one RGB frame. spread_s > 0 spreads the packets evenly over that many
    seconds instead of sending them as a single burst."""
    offsets = list(range(0, len(rgb), MAX_PAYLOAD))
    gap = spread_s / len(offsets) if spread_s > 0 else 0
    t = time.perf_counter()
    for off in offsets:
        chunk = rgb[off:off + MAX_PAYLOAD]
        last = off + len(chunk) >= len(rgb)
        flags = 0x40 | (0x01 if last else 0)  # version 1 + push on the last packet
        header = struct.pack(">BBBBIH", flags, seq & 0x0F, 0x0B, 0x01, off, len(chunk))
        sock.sendto(header + chunk, addr)
        if gap:
            t += gap
            wait_until(t)


def gradient(w, h, t):
    out = bytearray(w * h * 3)
    i = 0
    for y in range(h):
        for x in range(w):
            out[i] = int(127 + 127 * math.sin(x / 8 + t))
            out[i + 1] = int(127 + 127 * math.sin(y / 8 + t * 1.3))
            out[i + 2] = int(127 + 127 * math.sin((x + y) / 12 - t))
            i += 3
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description="DDP test sender")
    ap.add_argument("host")
    ap.add_argument("--port", type=int, default=4048)
    ap.add_argument("--size", default="64x64", help="frame size WxH (must match ddp_display)")
    ap.add_argument("--fps", type=float, default=30)
    ap.add_argument("--solid", help="hex color, e.g. ff0000 - send one frame and exit")
    ap.add_argument("--spread", type=float, default=0.5,
                    help="fraction of the frame time to spread packets over (0 = burst, default 0.5)")
    a = ap.parse_args()

    w, h = map(int, a.size.lower().split("x"))
    addr = (a.host, a.port)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    if a.solid:
        send_frame(sock, addr, bytes.fromhex(a.solid) * (w * h), 0)
        return

    period = 1 / a.fps
    seq, t0 = 0, time.perf_counter()
    next_t, stat_t, stat_n = t0, t0, 0
    try:
        while True:
            frame = gradient(w, h, time.perf_counter() - t0)
            send_frame(sock, addr, frame, seq, period * a.spread)
            seq += 1
            stat_n += 1
            now = time.perf_counter()
            if now - stat_t >= 1:
                print(f"sent {stat_n / (now - stat_t):.1f} fps", flush=True)
                stat_t, stat_n = now, 0
            next_t += period
            if next_t < now:  # falling behind - don't catch up with bursts
                next_t = now
            wait_until(next_t)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
