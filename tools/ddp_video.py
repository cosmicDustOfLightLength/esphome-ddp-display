#!/usr/bin/env python3
"""Play a video (YouTube URL or local file) on the display over DDP.

Requirements:
  pip install opencv-python numpy yt-dlp

Examples:
  python ddp_video.py 192.168.1.50 "https://www.youtube.com/watch?v=XXXXXXXXXXX"
  python ddp_video.py 192.168.1.50 video.mp4 --loop
  python ddp_video.py 192.168.1.50 URL --size 128x128 --fit cover --brightness 0.6
  python ddp_video.py 192.168.1.50 URL --start 30 --fps 25

A URL is downloaded once (video only, no audio, max 480p) into ddp_cache/ and
reused from disk on the next run. Only download content you have the right to use.
Ctrl+C to stop.
"""
import argparse
import os
import socket
import struct
import sys
import time

import cv2
import numpy as np

MAX_PAYLOAD = 1440
CACHE_DIR = "ddp_cache"


# ---------- DDP ----------

def wait_until(t):
    # time.sleep() on Windows has 1-15 ms resolution - busy-wait the last 2 ms
    while True:
        left = t - time.perf_counter()
        if left <= 0:
            return
        if left > 0.002:
            time.sleep(left - 0.002)


def send_frame(sock, addr, rgb: bytes, seq: int, spread_s: float = 0.0):
    """Send one RGB frame; spread_s > 0 spreads the packets over time instead of a burst."""
    offsets = range(0, len(rgb), MAX_PAYLOAD)
    gap = spread_s / len(offsets) if spread_s > 0 else 0
    t = time.perf_counter()
    for off in offsets:
        chunk = rgb[off:off + MAX_PAYLOAD]
        last = off + len(chunk) >= len(rgb)
        flags = 0x40 | (0x01 if last else 0)
        sock.sendto(struct.pack(">BBBBIH", flags, seq & 0x0F, 0x0B, 0x01, off, len(chunk)) + chunk, addr)
        if gap:
            t += gap
            wait_until(t)


# ---------- video source ----------

def is_url(s):
    return s.startswith(("http://", "https://"))


def download(url):
    try:
        import yt_dlp
    except ImportError:
        sys.exit("yt-dlp is missing: pip install yt-dlp")

    os.makedirs(CACHE_DIR, exist_ok=True)
    opts = {
        # Video only (no audio = no ffmpeg needed for merging), max 480p,
        # H.264 first because OpenCV can decode it everywhere.
        "format": "bv*[height<=480][vcodec^=avc1]/bv*[height<=480]/b[height<=480]/worst",
        "outtmpl": os.path.join(CACHE_DIR, "%(id)s.%(ext)s"),
        "noplaylist": True,
        "quiet": True,
        "no_warnings": True,
    }
    with yt_dlp.YoutubeDL(opts) as ydl:
        info = ydl.extract_info(url, download=False)
        path = ydl.prepare_filename(info)
        if os.path.exists(path):
            print(f"From cache: {path}")
        else:
            print(f"Downloading: {info.get('title', url)} ...")
            ydl.download([url])
        print(f"Title: {info.get('title')}  ({info.get('width')}x{info.get('height')}, {info.get('fps')} fps)")
        return path


# ---------- image ----------

def fit_frame(bgr, w, h, mode):
    sh, sw = bgr.shape[:2]
    if mode == "stretch":
        out = cv2.resize(bgr, (w, h), interpolation=cv2.INTER_AREA)
    elif mode == "cover":  # fill, crop the edges
        scale = max(w / sw, h / sh)
        nw, nh = max(w, round(sw * scale)), max(h, round(sh * scale))
        r = cv2.resize(bgr, (nw, nh), interpolation=cv2.INTER_AREA)
        x, y = (nw - w) // 2, (nh - h) // 2
        out = r[y:y + h, x:x + w]
    else:  # contain: fit the whole frame, black bars
        scale = min(w / sw, h / sh)
        nw, nh = max(1, round(sw * scale)), max(1, round(sh * scale))
        r = cv2.resize(bgr, (nw, nh), interpolation=cv2.INTER_AREA)
        out = np.zeros((h, w, 3), np.uint8)
        x, y = (w - nw) // 2, (h - nh) // 2
        out[y:y + nh, x:x + nw] = r
    return cv2.cvtColor(out, cv2.COLOR_BGR2RGB)


# ---------- playback ----------

def play(path, sock, addr, a, w, h):
    cap = cv2.VideoCapture(path)
    if not cap.isOpened():
        sys.exit(f"Cannot open: {path}")
    vfps = cap.get(cv2.CAP_PROP_FPS) or 30.0
    if vfps <= 0 or vfps > 240:
        vfps = 30.0
    out_fps = min(vfps, a.fps)
    period = 1.0 / out_fps
    if a.start > 0:
        cap.set(cv2.CAP_PROP_POS_MSEC, a.start * 1000)

    total = cap.get(cv2.CAP_PROP_FRAME_COUNT)
    print(f"Video {vfps:.2f} fps -> sending {out_fps:.1f} fps, {w}x{h}, fit={a.fit}"
          + (f", length {total / vfps:.0f} s" if total > 0 else ""))

    t0 = time.perf_counter()
    first_pos = cap.get(cv2.CAP_PROP_POS_MSEC) / 1000.0
    next_send = t0
    seq = sent = skipped = 0
    stat_t = t0
    while True:
        now = time.perf_counter()
        # which moment of the video should be on screen right now
        want = first_pos + (now - t0)
        # catch up: skip frames that are already in the past (grab = no decoding)
        pos = cap.get(cv2.CAP_PROP_POS_MSEC) / 1000.0
        while pos + 1.0 / vfps < want - period:
            if not cap.grab():
                return
            skipped += 1
            pos = cap.get(cv2.CAP_PROP_POS_MSEC) / 1000.0

        ok, bgr = cap.read()
        if not ok:
            return
        frame_t = cap.get(cv2.CAP_PROP_POS_MSEC) / 1000.0
        # video fps > send fps: skip frames between send ticks
        if frame_t - first_pos < (next_send - t0) - 1.0 / vfps:
            continue

        rgb = fit_frame(bgr, w, h, a.fit)
        if a.brightness != 1.0:
            rgb = np.clip(rgb.astype(np.float32) * a.brightness, 0, 255).astype(np.uint8)

        wait_until(t0 + (frame_t - first_pos))
        send_frame(sock, addr, rgb.tobytes(), seq, period * a.spread)
        seq += 1
        sent += 1
        next_send += period

        now = time.perf_counter()
        if now - stat_t >= 2:
            print(f"\r{frame_t:7.1f} s  sending {sent / (now - stat_t):4.1f} fps  skipped {skipped}   ",
                  end="", flush=True)
            stat_t, sent = now, 0


def main():
    ap = argparse.ArgumentParser(description="Play a YouTube video / video file on the display over DDP")
    ap.add_argument("host")
    ap.add_argument("source", help="URL (YouTube etc.) or path to a video file")
    ap.add_argument("--port", type=int, default=4048)
    ap.add_argument("--size", default="128x128", help="frame size WxH, must match ddp_display (default 128x128)")
    ap.add_argument("--fit", choices=["contain", "cover", "stretch"], default="cover")
    ap.add_argument("--fps", type=float, default=30, help="max send fps (default 30)")
    ap.add_argument("--brightness", type=float, default=1.0, help="brightness multiplier 0..1")
    ap.add_argument("--start", type=float, default=0, help="start at second")
    ap.add_argument("--loop", action="store_true", help="loop playback")
    ap.add_argument("--spread", type=float, default=0.5, help="spread packets over this fraction of the frame time (0 = burst)")
    a = ap.parse_args()

    w, h = map(int, a.size.lower().split("x"))
    path = download(a.source) if is_url(a.source) else a.source
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    addr = (a.host, a.port)
    try:
        while True:
            play(path, sock, addr, a, w, h)
            if not a.loop:
                break
            a.start = 0
            print("\n-- restarting --")
    except KeyboardInterrupt:
        pass
    print("\nDone.")


if __name__ == "__main__":
    main()
