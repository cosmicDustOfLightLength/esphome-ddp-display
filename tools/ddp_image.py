#!/usr/bin/env python3
"""Send a static image (or a test pattern) to the display over DDP.

  python ddp_image.py 192.168.1.50 picture.png              # image (requires: pip install pillow)
  python ddp_image.py 192.168.1.50 --pattern                # test pattern (no Pillow needed)
  python ddp_image.py 192.168.1.50 logo.png --fit cover     # crop instead of fit
  python ddp_image.py 192.168.1.50 logo.png --once          # send once and exit

By default the image is re-sent every 2 s so that Auto mode does not fall back to
the normal display (default timeout 5 s). With --once it is sent a single time -
set the display mode to "DDP" in that case. Ctrl+C to stop.
"""
import argparse
import socket
import struct
import time

MAX_PAYLOAD = 1440


def send_frame(sock, addr, rgb: bytes, seq: int = 0):
    for off in range(0, len(rgb), MAX_PAYLOAD):
        chunk = rgb[off:off + MAX_PAYLOAD]
        last = off + len(chunk) >= len(rgb)
        flags = 0x40 | (0x01 if last else 0)
        sock.sendto(struct.pack(">BBBBIH", flags, seq & 0x0F, 0x0B, 0x01, off, len(chunk)) + chunk, addr)


def load_image(path, w, h, fit):
    from PIL import Image, ImageOps  # pip install pillow

    img = Image.open(path)
    img = ImageOps.exif_transpose(img).convert("RGBA")
    bg = Image.new("RGBA", img.size, (0, 0, 0, 255))       # transparency -> black background
    img = Image.alpha_composite(bg, img).convert("RGB")
    if fit == "cover":
        img = ImageOps.fit(img, (w, h), Image.LANCZOS)         # fill, crop the edges
    elif fit == "stretch":
        img = img.resize((w, h), Image.LANCZOS)                # stretch
    else:
        img = ImageOps.pad(img, (w, h), Image.LANCZOS, color=(0, 0, 0))  # fit whole image, black bars
    return img.tobytes()


def test_pattern(w, h):
    """Color bars on top, 8 px grid + gray ramp below, white border - glitches are easy to spot."""
    bars = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0),
            (255, 0, 255), (255, 0, 0), (0, 0, 255), (40, 40, 40)]
    out = bytearray()
    for y in range(h):
        for x in range(w):
            if x in (0, w - 1) or y in (0, h - 1):
                c = (255, 255, 255)
            elif y < h // 2:
                c = bars[x * len(bars) // w]
            elif x % 8 == 0 or y % 8 == 0:
                c = (0, 80, 0)
            else:
                v = x * 255 // (w - 1)
                c = (v, v, v)
            out += bytes(c)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description="Send a static image over DDP")
    ap.add_argument("host")
    ap.add_argument("image", nargs="?")
    ap.add_argument("--pattern", action="store_true", help="send a test pattern instead of an image")
    ap.add_argument("--port", type=int, default=4048)
    ap.add_argument("--size", default="64x64", help="frame size WxH (must match ddp_display)")
    ap.add_argument("--fit", choices=["contain", "cover", "stretch"], default="contain")
    ap.add_argument("--once", action="store_true", help="send once and exit")
    ap.add_argument("--every", type=float, default=2.0, help="re-send interval in seconds (default 2)")
    a = ap.parse_args()

    w, h = map(int, a.size.lower().split("x"))
    if a.pattern or not a.image:
        rgb = test_pattern(w, h)
    else:
        rgb = load_image(a.image, w, h, a.fit)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    addr = (a.host, a.port)
    send_frame(sock, addr, rgb)
    print(f"Sent {w}x{h} ({len(rgb)} B) to {a.host}:{a.port}")
    if a.once:
        return
    try:
        seq = 1
        while True:
            time.sleep(a.every)
            send_frame(sock, addr, rgb, seq)
            seq += 1
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
