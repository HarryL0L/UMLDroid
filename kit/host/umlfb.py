#!/usr/bin/env python3
"""Host-side helper for an Android-on-UML guest.

  umlfb.py png FRAMEFILE OUT.png         save the current exported frame
  umlfb.py info FRAMEFILE                print the frame header
  umlfb.py tap FIFO X Y                  tap at (X, Y)
  umlfb.py swipe FIFO X1 Y1 X2 Y2 [MS]   swipe
  umlfb.py key FIFO CODE                 press and release a Linux key code
                                         (158 back, 172 home, 580 recents, 116 power)

FRAMEFILE is the host file given to the kernel as vkms.export=, FIFO the one
given as umltouch=. See drivers/gpu/drm/vkms/vkms_export.c and
arch/um/drivers/uml_touch.c for the formats.
"""
import mmap, os, struct, sys, time, zlib

HDR = struct.Struct("<6I2Q")   # magic version width height stride format seq frames
MAGIC = 0x42464D55


def read_frame(path, tries=50):
    with open(path, "rb") as f:
        m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        for _ in range(tries):
            magic, ver, w, h, stride, fmt, seq, frames = HDR.unpack_from(m, 0)
            if magic != MAGIC:
                raise SystemExit("no frame exported yet")
            if seq & 1:
                time.sleep(0.01)
                continue
            data = m[4096:4096 + stride * h]
            if HDR.unpack_from(m, 0)[6] == seq:
                return w, h, stride, frames, data
        raise SystemExit("frame kept changing")


def write_png(out, w, h, stride, data):
    raw = b"".join(b"\0" + data[y * stride:y * stride + w * 4] for y in range(h))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
    with open(out, "wb") as f:
        f.write(png)


def send(fifo, *events):
    fd = os.open(fifo, os.O_WRONLY)
    try:
        for ev in events:
            if ev == "sleep":
                continue
            os.write(fd, struct.pack("<Iii", *ev))
    finally:
        os.close(fd)


def main(a):
    cmd = a[0]
    if cmd == "info":
        w, h, stride, frames, _ = read_frame(a[1])
        print(f"{w}x{h} stride {stride}, {frames} frames")
    elif cmd == "png":
        w, h, stride, frames, data = read_frame(a[1])
        write_png(a[2], w, h, stride, data)
        print(f"{a[2]}: {w}x{h}, frame {frames}")
    elif cmd == "tap":
        x, y = int(a[2]), int(a[3])
        send(a[1], (1, x, y))
        time.sleep(0.08)
        send(a[1], (0, 0, 0))
    elif cmd == "swipe":
        x1, y1, x2, y2 = map(int, a[2:6])
        ms = int(a[6]) if len(a) > 6 else 300
        steps = max(ms // 16, 2)
        for i in range(steps + 1):
            send(a[1], (1, x1 + (x2 - x1) * i // steps, y1 + (y2 - y1) * i // steps))
            time.sleep(ms / 1000 / steps)
        send(a[1], (0, 0, 0))
    elif cmd == "key":
        send(a[1], (2, int(a[2]), 1))
        time.sleep(0.05)
        send(a[1], (2, int(a[2]), 0))
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:] or ["help"])
