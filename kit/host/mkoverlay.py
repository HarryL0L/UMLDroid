#!/usr/bin/env python3
"""Write an uncompressed newc cpio with the files given as DEST=SRC[:MODE]
(MODE in octal, default 0644); directories on the way are created.
Usage: mkoverlay.py OUT.cpio first_stage.sh=guest/first_stage.sh:755 ..."""
import sys


def entry(name, data, mode, ino):
    name_b = name.encode() + b"\0"
    hdr = "070701" + "".join("%08X" % v for v in (
        ino, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(name_b), 0))
    out = hdr.encode() + name_b
    out += b"\0" * (-len(out) % 4) + data
    return out + b"\0" * (-len(out) % 4)


out, specs = sys.argv[1], sys.argv[2:]
cpio, ino, dirs = b"", 1, set()
for spec in specs:
    dest, src = spec.split("=", 1)
    mode = 0o644
    if ":" in src:
        src, m = src.rsplit(":", 1)
        mode = int(m, 8)
    parts = dest.split("/")
    for i in range(1, len(parts)):
        d = "/".join(parts[:i])
        if d not in dirs:
            dirs.add(d)
            cpio += entry(d, b"", 0o040755, ino)
            ino += 1
    cpio += entry(dest, open(src, "rb").read(), 0o100000 | mode, ino)
    ino += 1
cpio += entry("TRAILER!!!", b"", 0, 0)
cpio += b"\0" * (-len(cpio) % 512)
open(out, "wb").write(cpio)
