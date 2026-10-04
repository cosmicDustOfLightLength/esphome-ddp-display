#!/usr/bin/env python3
"""Tests for the PC-side tools: packets they send are received on 127.0.0.1
and reassembled into frames exactly like the ESP component does.
Pillow / OpenCV tests are skipped when those libraries are not installed."""
import os
import socket
import struct
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))

failures = 0


def check(cond, msg):
    global failures
    print(("  ok   " if cond else "  FAIL ") + msg)
    if not cond:
        failures += 1


class Receiver:
    """Collects DDP frames (like ddp_display: write at offset, show on push)."""

    def __init__(self, port, size):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
        self.sock.bind(("127.0.0.1", port))
        self.sock.settimeout(0.5)
        self.buf = bytearray(size)
        self.frames, self.packets = [], 0
        self.run = True
        self.t = threading.Thread(target=self._loop, daemon=True)
        self.t.start()

    def _loop(self):
        while self.run:
            try:
                p, _ = self.sock.recvfrom(2000)
            except socket.timeout:
                continue
            flags, _, _, did, off, ln = struct.unpack(">BBBBIH", p[:10])
            assert flags & 0xC0 == 0x40 and did == 1 and ln == len(p) - 10
            self.buf[off:off + ln] = p[10:]
            self.packets += 1
            if flags & 1:
                self.frames.append(bytes(self.buf))

    def close(self):
        time.sleep(0.2)
        self.run = False
        self.t.join()
        self.sock.close()


def test_ddp_test():
    print("[ddp_test.py]")
    import ddp_test
    rx = Receiver(15001, 128 * 128 * 3)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    f1 = ddp_test.gradient(128, 128, 0.0)
    ddp_test.send_frame(s, ("127.0.0.1", 15001), f1, 0)
    t = time.perf_counter()
    ddp_test.send_frame(s, ("127.0.0.1", 15001), ddp_test.gradient(128, 128, 1.0), 1, spread_s=0.02)
    spread_took = time.perf_counter() - t
    rx.close()
    check(len(rx.frames) == 2 and rx.packets == 70, "2 frames x 35 packets (128x128)")
    check(rx.frames[0] == f1, "frame reassembled byte-exact")
    check(0.015 < spread_took < 0.1, f"--spread paces packets ({spread_took * 1000:.0f} ms for 20 ms)")


def test_ddp_image():
    print("[ddp_image.py]")
    import ddp_image
    pat = ddp_image.test_pattern(64, 64)
    check(len(pat) == 64 * 64 * 3, "test pattern size")
    rx = Receiver(15002, 64 * 64 * 3)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    ddp_image.send_frame(s, ("127.0.0.1", 15002), pat)
    rx.close()
    check(rx.frames and rx.frames[0] == pat, "pattern delivered byte-exact")
    try:
        from PIL import Image
    except ImportError:
        print("  skip image loading (no Pillow)")
        return
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "x.png")
        Image.new("RGBA", (300, 200), (255, 0, 0, 128)).save(p)
        rgb = ddp_image.load_image(p, 64, 64, "contain")
        check(len(rgb) == 64 * 64 * 3, "PNG scaled to 64x64")
        check(rgb[32 * 64 * 3:32 * 64 * 3 + 3] == bytes((128, 0, 0)), "alpha blended onto black")
        check(rgb[:3] == bytes((0, 0, 0)), "contain = black bars")


def test_ddp_video():
    print("[ddp_video.py]")
    try:
        import cv2
        import numpy as np
    except ImportError:
        print("  skip (no opencv-python / numpy)")
        return
    import ddp_video
    img = np.zeros((360, 640, 3), np.uint8)
    for mode in ("contain", "cover", "stretch"):
        out = ddp_video.fit_frame(img, 128, 128, mode)
        check(out.shape == (128, 128, 3), f"fit={mode} -> 128x128")

    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "t.mp4")
        vw = cv2.VideoWriter(path, cv2.VideoWriter_fourcc(*"mp4v"), 60, (320, 180))
        for i in range(120):  # 2 s @ 60 fps
            f = np.zeros((180, 320, 3), np.uint8)
            f[:] = (i * 2 % 256, 40, 200)
            vw.write(f)
        vw.release()

        class A:
            fps, fit, brightness, start, spread = 30, "cover", 1.0, 0, 0.5

        rx = Receiver(15003, 128 * 128 * 3)
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        t = time.perf_counter()
        ddp_video.play(path, s, ("127.0.0.1", 15003), A, 128, 128)
        took = time.perf_counter() - t
        rx.close()
        print()
        check(55 <= len(rx.frames) <= 62, f"60 fps video sent at 30 fps ({len(rx.frames)} frames)")
        check(1.8 < took < 2.6, f"real-time playback ({took:.2f} s for 2 s video)")


if __name__ == "__main__":
    test_ddp_test()
    test_ddp_image()
    test_ddp_video()
    print(f"\n{'FAILED' if failures else 'ALL OK'} ({failures} failure{'s' if failures != 1 else ''})")
    sys.exit(1 if failures else 0)
